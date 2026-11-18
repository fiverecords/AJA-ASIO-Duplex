// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Windows only: one program at a time on a card's audio system.

#include "test_framework.h"

#if defined(_WIN32)

#include "asio/win/InstanceLock.h"

using ajaduplex::asio::win::InstanceLock;
using ajaduplex::asio::win::TaskModeMark;

TEST_CASE(InstanceLock, SecondHolderIsRefusedAndToldWho)
{
    InstanceLock first;
    InstanceLock second;
    std::string holder;
    ASSERT_TRUE(first.acquire(9, 2, holder));
    EXPECT_FALSE(second.acquire(9, 2, holder));
    EXPECT_CONTAINS(holder, "duplex-tests");
    EXPECT_CONTAINS(holder, "(process ");
    // Another audio system, or another card, is free.
    InstanceLock other;
    EXPECT_TRUE(other.acquire(9, 1, holder));
    InstanceLock otherCard;
    EXPECT_TRUE(otherCard.acquire(8, 2, holder));
    // Once let go, the next one gets it.
    first.release();
    EXPECT_TRUE(second.acquire(9, 2, holder));
}

TEST_CASE(InstanceLock, SaysWhoHoldsItWithoutTakingIt)
{
    std::string holder;
    EXPECT_FALSE(InstanceLock::heldBy(9, 4, holder));
    InstanceLock lock;
    ASSERT_TRUE(lock.acquire(9, 4, holder));
    holder.clear();
    ASSERT_TRUE(InstanceLock::heldBy(9, 4, holder));
    EXPECT_CONTAINS(holder, "duplex-tests");
    EXPECT_CONTAINS(holder, "(process ");
    EXPECT_FALSE(InstanceLock::heldBy(9, 5, holder));   // another audio system
    // Asking took nothing: once let go, it is free.
    lock.release();
    EXPECT_FALSE(InstanceLock::heldBy(9, 4, holder));
    InstanceLock next;
    EXPECT_TRUE(next.acquire(9, 4, holder));
}

TEST_CASE(InstanceLock, HeldByAProgramThatCannotBeAsked)
{
    // A host run with other rights (as administrator, say) makes a lock that
    // this program may not open: an empty access list stands in for that.
    SECURITY_DESCRIPTOR descriptor;
    ASSERT_TRUE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != FALSE);
    ACL nobody;
    ASSERT_TRUE(InitializeAcl(&nobody, sizeof(nobody), ACL_REVISION) != FALSE);
    ASSERT_TRUE(SetSecurityDescriptorDacl(&descriptor, TRUE, &nobody, FALSE) != FALSE);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
    const wchar_t* name = L"Local\\AJADuplexASIO-card9-as6";
    HANDLE theirs = CreateMutexW(&attributes, FALSE, name);
    ASSERT_TRUE(theirs != nullptr);
    HANDLE tried = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (tried == nullptr)
    {
        std::string holder;
        EXPECT_TRUE(InstanceLock::heldBy(9, 6, holder));
        EXPECT_EQ(holder, std::string("another program"));
    }
    else
    {
        CloseHandle(tried);   // a system that does not check the list: nothing to see here
    }
    CloseHandle(theirs);
}

TEST_CASE(InstanceLock, GoesWithTheObject)
{
    std::string holder;
    {
        InstanceLock scoped;
        ASSERT_TRUE(scoped.acquire(9, 3, holder));
    }
    InstanceLock again;
    EXPECT_TRUE(again.acquire(9, 3, holder));
}

TEST_CASE(TaskModeMark, AnotherProgramSeesItWhileItIsSet)
{
    // What AJA Input does to ask (its src/DuplexAsioPresence.cpp): open
    // the mutex by name. The name is the agreement between the two.
    EXPECT_TRUE(TaskModeMark::name(7) == L"Local\\AJADuplexASIO-card7-oem");
    auto asked = [](unsigned card) {
        HANDLE mark = OpenMutexW(SYNCHRONIZE, FALSE, TaskModeMark::name(card).c_str());
        if (mark == nullptr)
            return false;
        CloseHandle(mark);
        return true;
    };
    EXPECT_FALSE(asked(7));
    TaskModeMark mark;
    EXPECT_FALSE(mark.isSet());
    mark.set(7);
    EXPECT_TRUE(mark.isSet());
    EXPECT_TRUE(asked(7));
    EXPECT_TRUE(TaskModeMark::present(7));
    EXPECT_FALSE(asked(6));   // another card's is another name
    mark.clear();
    EXPECT_FALSE(mark.isSet());
    EXPECT_FALSE(asked(7));
    EXPECT_FALSE(TaskModeMark::present(7));
    mark.clear();   // again: nothing to do
}

TEST_CASE(TaskModeMark, LastsUntilTheLastHolderLetsGo)
{
    // Two drivers on one card (two programs, or two audio systems): the
    // mark is there while either answers for the task mode.
    TaskModeMark first;
    TaskModeMark second;
    first.set(7);
    second.set(7);
    first.clear();
    EXPECT_TRUE(TaskModeMark::present(7));
    second.clear();
    EXPECT_FALSE(TaskModeMark::present(7));
    {
        TaskModeMark scoped;
        scoped.set(7);
        EXPECT_TRUE(TaskModeMark::present(7));
    }
    EXPECT_FALSE(TaskModeMark::present(7));   // it goes with the object
}

#endif

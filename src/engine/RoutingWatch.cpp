// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#include "engine/RoutingWatch.h"

#include <cstdio>

namespace ajaduplex::engine
{

namespace routing = device::routing;

bool RoutingWatch::takeBaseline(unsigned owned, unsigned alwaysKept, bool tookFromServices)
{
    device::CardRouting now;
    active_ = card_.readRouting(now);
    baseline_ = active_ ? now : device::CardRouting{};
    last_ = baseline_;
    owned_ = owned & routing::kWritable;
    alwaysKept_ = alwaysKept & owned_;
    // A pass the services were in the middle of when the driver took the
    // task mode may still write after this reading: the first looks see to
    // it, as after a task mode found changed.
    afterModeChange_ = tookFromServices && (alwaysKept_ & routing::kTaskMode) != 0 ? kChecksAfterModeChange : 0;
    return active_;
}

bool RoutingWatch::change(const device::CardRouting& wanted, unsigned parts)
{
    parts &= routing::kWritable;
    if (!active_ || parts == 0)
        return false;
    card_.writeRouting(wanted, parts);
    device::CardRouting after;
    if (!card_.readRouting(after))
        return false;
    // Ours from now on, as the card holds them; a source rewrites the select.
    const unsigned taken = (parts & routing::kSource) != 0 ? parts | routing::kXpandSelect : parts;
    device::copyRoutingParts(baseline_, after, taken);
    device::copyRoutingParts(last_, after, taken);
    owned_ |= parts;
    return (device::routingDifferences(wanted, after) & parts) == 0;
}

RoutingWatch::Check RoutingWatch::check(bool keep)
{
    Check c;
    if (!active_)
        return c;
    device::CardRouting now;
    if (!card_.readRouting(now))
        return c;
    c.read = true;
    c.before = last_;
    c.now = now;
    c.changed = device::routingDifferences(last_, now);

    // Only what was read both times can be told apart, and put back.
    const unsigned comparable = device::routingParts(baseline_) & device::routingParts(now);
    c.notOurs = device::routingDifferences(baseline_, now) & comparable & owned_;
    // The task mode the driver holds, found changed: AJA's services had the
    // card meanwhile, and in standard mode they move the shared routing
    // back to their own within a second (Q9). The mode alone would leave
    // the driver's outputs on their mixer, so what is not ours at this look
    // and at the next goes back with it, kept or not.
    if ((c.notOurs & alwaysKept_ & routing::kTaskMode) != 0)
        afterModeChange_ = kChecksAfterModeChange;
    const bool servicesRan = afterModeChange_ > 0;
    if (servicesRan)
        --afterModeChange_;
    const unsigned keeping = c.notOurs & (keep || servicesRan ? owned_ : alwaysKept_);
    if (keeping != 0)
    {
        // Our source, set the SDK's way, rewrites the select: ours goes
        // back with it.
        unsigned write = keeping;
        if ((write & routing::kSource) != 0)
            write |= routing::kXpandSelect & comparable;
        card_.writeRouting(baseline_, write);
        c.putBack = keeping;
        device::CardRouting after;
        if (card_.readRouting(after))
        {
            c.putBackFailed = device::routingDifferences(baseline_, after) & c.putBack;
            now = after;
        }
        else
        {
            c.putBackFailed = c.putBack;   // cannot tell: counted as not taken
        }
    }
    last_ = now;
    return c;
}

const char* routingPartName(unsigned part)
{
    switch (part)
    {
        case routing::kSource:           return "input source";
        case routing::kXpandSelect:      return "Xpand input select";
        case routing::kAnalogLevel:      return "Xpand analog level";
        case routing::kAesGroup << 0:    return "AES out 1-4";
        case routing::kAesGroup << 1:    return "AES out 5-8";
        case routing::kAesGroup << 2:    return "AES out 9-12";
        case routing::kAesGroup << 3:    return "AES out 13-16";
        case routing::kMonitor:          return "monitor";
        case routing::kHdmiOut:          return "HDMI out";
        case routing::kTaskMode:         return "task mode";
        default:                         break;
    }
    static const char* const kSdiNames[device::CardRouting::kMaxSdiOutputs] = {
        "SDI out 1", "SDI out 2", "SDI out 3", "SDI out 4", "SDI out 5", "SDI out 6", "SDI out 7", "SDI out 8"};
    for (unsigned n = 0; n < device::CardRouting::kMaxSdiOutputs; ++n)
    {
        if ((routing::kSdiOut << n) == part)
            return kSdiNames[n];
    }
    return "?";
}

namespace
{

void describeSource(device::OutputSource source, unsigned width, char* buffer, std::size_t size)
{
    std::snprintf(buffer, size, "audio system %u, channels %u-%u", source.audioSystem, source.firstChannel,
                  source.firstChannel + width - 1);
}

} // namespace

const char* describeRoutingPart(const device::CardRouting& r, unsigned part, char* buffer, std::size_t size)
{
    if (buffer == nullptr || size == 0)
        return "";
    if (part == 0 || (part & (part - 1)) != 0)
    {
        std::snprintf(buffer, size, "?");
        return buffer;
    }
    if ((device::routingParts(r) & part) == 0)
    {
        std::snprintf(buffer, size, "unreadable");
        return buffer;
    }
    switch (part)
    {
        case routing::kSource:
            if (r.source == device::InputSource::Embedded)
                std::snprintf(buffer, size, "SDI %u embedded", r.embeddedInput + 1);
            else if (r.source == device::InputSource::Other)
                std::snprintf(buffer, size, "another source (code %u)", r.sourceCode);
            else
                std::snprintf(buffer, size, "%s", device::toString(r.source));
            break;
        case routing::kXpandSelect:
            std::snprintf(buffer, size, "%u%s", r.xpandSelect,
                          r.xpandSelect == 0 ? " (AES)" : r.xpandSelect == 1 ? " (analog)" : "");
            break;
        case routing::kAnalogLevel:
            if (r.analogLevelDbu != 0)
                std::snprintf(buffer, size, "%+d dBu", r.analogLevelDbu);
            else
                std::snprintf(buffer, size, "a value without a name (code %u)", r.analogLevelCode);
            break;
        case routing::kMonitor:
            describeSource(r.monitor, 2, buffer, size);
            break;
        case routing::kHdmiOut:
            describeSource(device::OutputSource{r.hdmiOut.audioSystem, r.hdmiOut.firstChannel},
                           r.hdmiOut.channels, buffer, size);
            break;
        case routing::kTaskMode:
            std::snprintf(buffer, size, "%s", device::toString(r.taskMode));
            break;
        default:
        {
            std::snprintf(buffer, size, "?");
            for (unsigned group = 0; group < device::CardRouting::kMaxAesGroups; ++group)
            {
                if ((routing::kAesGroup << group) == part)
                    describeSource(r.aes[group], 4, buffer, size);
            }
            for (unsigned n = 0; n < device::CardRouting::kMaxSdiOutputs; ++n)
            {
                if ((routing::kSdiOut << n) != part)
                    continue;
                // All the audio system's channels, embedded.
                if (r.sdiOut[n].enabled)
                    std::snprintf(buffer, size, "audio system %u", r.sdiOut[n].audioSystem);
                else
                    std::snprintf(buffer, size, "no audio (its embedder is off; audio system %u)",
                                  r.sdiOut[n].audioSystem);
            }
            break;
        }
    }
    return buffer;
}

std::string describeRouting(const device::CardRouting& r)
{
    const unsigned parts = device::routingParts(r);
    std::string text;
    char value[96];
    for (unsigned part = 1; part <= routing::kLast; part <<= 1)
    {
        if ((parts & part) == 0)
            continue;
        if (!text.empty())
            text += "; ";
        text += std::string(routingPartName(part)) + ": " + describeRoutingPart(r, part, value, sizeof(value));
    }
    return text.empty() ? std::string("nothing could be read") : text;
}

namespace
{

/// What a change of the Xpand's select means for the driver's input.
const char* selectHint(const device::CardRouting& ours, const device::CardRouting& now)
{
    if (ours.xpandSelect == 1 && now.xpandSelect == 0)
        return ": the analog inputs are off";
    if (ours.xpandSelect == 0 && now.xpandSelect != 0)
        return ": some AES inputs may hear the analog ones instead";
    return "";
}

} // namespace

unsigned reportCheck(const RoutingWatch& watch, const RoutingWatch::Check& c, const char* keepName,
                     void (*emit)(void* context, const char* line), void* context)
{
    if (!c.read)
        return 0;
    const device::CardRouting& ours = watch.baseline();
    const unsigned ourParts = device::routingParts(ours) & watch.owned();
    const unsigned nowParts = device::routingParts(c.now);
    const unsigned differs = device::routingDifferences(ours, c.now);
    char now[96];
    char was[96];
    char line[400];
    unsigned changes = 0;
    for (unsigned part = 1; part <= routing::kLast; part <<= 1)
    {
        const char* name = routingPartName(part);
        line[0] = '\0';
        if ((c.changed & part) != 0)
        {
            describeRoutingPart(c.now, part, now, sizeof(now));
            if ((nowParts & part) == 0)
                std::snprintf(line, sizeof(line), "routing: %s can no longer be read", name);
            else if ((ourParts & part) == 0)
            {
                // Not the driver's (the task mode, the level it leaves as
                // the card has it): only said.
                describeRoutingPart(c.before, part, was, sizeof(was));
                std::snprintf(line, sizeof(line), "routing: %s%s is now %s (it was %s)",
                              part == routing::kTaskMode ? "the card's " : "", name, now, was);
            }
            else if ((differs & part) == 0)
                std::snprintf(line, sizeof(line), "routing: %s is the driver's again: %s", name, now);
            else
            {
                ++changes;
                describeRoutingPart(ours, part, was, sizeof(was));
                char keep[64];
                std::snprintf(keep, sizeof(keep), "left as it is (%s is off)", keepName);
                std::snprintf(line, sizeof(line), "routing: another program changed %s to %s; the driver's is %s%s; %s",
                              name, now, was, part == routing::kXpandSelect ? selectHint(ours, c.now) : "",
                              (c.putBack & part) != 0 ? "put back" : keep);
            }
        }
        else if ((c.putBack & part) != 0)
        {
            describeRoutingPart(ours, part, was, sizeof(was));
            std::snprintf(line, sizeof(line), "routing: %s put back: %s", name, was);
        }
        if (line[0] != '\0')
            emit(context, line);
        if ((c.putBackFailed & part) != 0)
        {
            std::snprintf(line, sizeof(line), "routing: %s could not be put back", name);
            emit(context, line);
        }
    }
    return changes;
}

std::string routingPartNames(unsigned parts)
{
    std::string text;
    for (unsigned part = 1; part <= routing::kLast; part <<= 1)
    {
        if ((parts & part) == 0)
            continue;
        if (!text.empty())
            text += ", ";
        text += routingPartName(part);
    }
    return text;
}

} // namespace ajaduplex::engine

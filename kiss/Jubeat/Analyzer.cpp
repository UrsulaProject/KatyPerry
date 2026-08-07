#include "Jubeat/Analyzer.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace jbt {

namespace {

constexpr double kSectorMilliseconds = 1000.0 / 300.0;

const char *EventTypeName(const SequenceEventType type) {
  switch (type) {
  case EVENT_NONE:
    return "None";
  case EVENT_PLAY:
    return "Play";
  case EVENT_END:
    return "End";
  case EVENT_MEASURE:
    return "Measure";
  case EVENT_HAKU:
    return "Haku";
  case EVENT_TEMPO:
    return "Tempo";
  case EVENT_LONG:
    return "Long";
  }

  return "Unknown";
}

const char *JudgeResultName(const JudgeResult result) {
  switch (result) {
  case JUDGE_NONE:
    return "None";
  case JUDGE_MISS:
    return "Miss";
  case JUDGE_POOR:
    return "Poor";
  case JUDGE_GOOD:
    return "Good";
  case JUDGE_GREAT:
    return "Great";
  case JUDGE_PERFECT:
    return "Perfect";
  }

  return "Unknown";
}

double SectorToMs(const int32_t sector) {
  return static_cast<double>(sector) * kSectorMilliseconds;
}

} // namespace

void Statistics::Add(const uint32_t index, const SequenceEventType type,
                     const JudgeResult result, const int32_t errorSector,
                     const uint32_t panel, const bool release) {
  if (result == JUDGE_NONE)
    return;

  switch (result) {
  case JUDGE_MISS:
    ++miss;
    break;

  case JUDGE_POOR:
    ++poor;
    break;

  case JUDGE_GOOD:
    ++good;
    break;

  case JUDGE_GREAT:
    ++great;
    break;

  case JUDGE_PERFECT:
    ++perfect;
    break;

  case JUDGE_NONE:
    return;
  }

  totalErrorSector += errorSector;

  maxEarly = std::min(maxEarly, errorSector);
  maxLate = std::max(maxLate, errorSector);

  records.push_back({
      index,
      type,
      result,
      errorSector,
      panel,
      release,
  });
}

uint32_t Statistics::JudgeCount() const {
  return miss + poor + good + great + perfect;
}

double Statistics::AverageErrorMs() const {
  if (records.empty())
    return 0.0;

  return static_cast<double>(totalErrorSector) /
         static_cast<double>(records.size()) * kSectorMilliseconds;
}

Analyzer::Analyzer(std::vector<SequenceEvent> initialEvents)
    : events(std::move(initialEvents)) {
  oldJudge.resize(events.size(), JUDGE_NONE);
  oldHoldHeadJudge.resize(events.size(), JUDGE_NONE);

  CountEvents();
  BuildRegions();
}

void Analyzer::CountEvents() {
  std::fill(std::begin(stats.eventCount), std::end(stats.eventCount), 0u);

  constexpr size_t kEventTypeCount =
      sizeof(stats.eventCount) / sizeof(stats.eventCount[0]);

  for (const SequenceEvent &event : events) {
    const auto type = static_cast<uint32_t>(event.type);

    if (type < kEventTypeCount)
      ++stats.eventCount[type];
  }
}

uint32_t Analyzer::GetPanel(const SequenceEvent &event) {
  //
  // Both PLAY and LONG encode the panel in bits [3:0].
  //
  return event.eve.raw & 0xF;
}

uint32_t Analyzer::GetHoldDuration(const SequenceEvent &event) {
  //
  // LONG duration is stored in bits [31:8].
  //
  return event.eve.raw >> 8;
}

void Analyzer::BeforeJudge(const std::vector<SequenceEvent> &currentEvents) {
    recentJudges.clear();
  //
  // A Sequence's event count is invariant during a song.
  //
  // If this changes, treating the current call as a fresh baseline is safer
  // than producing bogus transitions.
  //
  if (currentEvents.size() != events.size()) {
    events = currentEvents;

    oldJudge.assign(events.size(), JUDGE_NONE);
    oldHoldHeadJudge.assign(events.size(), JUDGE_NONE);

    for (size_t i = 0; i < events.size(); ++i) {
      oldJudge[i] = events[i].judge;
      oldHoldHeadJudge[i] = events[i].holdHeadJudge;
    }

    return;
  }

  events = currentEvents;

  for (size_t i = 0; i < events.size(); ++i) {
    oldJudge[i] = events[i].judge;
    oldHoldHeadJudge[i] = events[i].holdHeadJudge;
  }
}

void Analyzer::AfterJudge(const std::vector<SequenceEvent> &currentEvents,
                          const uint32_t currentSector) {
  if (currentEvents.size() != events.size()) {
    //
    // This should not happen inside one Sequence. Do not compare snapshots
    // with incompatible event indices.
    //
    events = currentEvents;

    oldJudge.resize(events.size(), JUDGE_NONE);
    oldHoldHeadJudge.resize(events.size(), JUDGE_NONE);

    return;
  }

  for (size_t i = 0; i < currentEvents.size(); ++i) {
    const SequenceEvent &event = currentEvents[i];

    switch (event.type) {
    case EVENT_PLAY: {
      //
      // PLAY stores its judgement at +0x02.
      //
      if (oldJudge[i] == JUDGE_NONE && event.judge != JUDGE_NONE) {
        const int32_t error = static_cast<int32_t>(currentSector) -
                              static_cast<int32_t>(event.sector);

        RecordJudge(static_cast<uint32_t>(i), EVENT_PLAY, event.judge, error,
                  GetPanel(event), false);
      }

      break;
    }

    case EVENT_LONG: {
      //
      // LONG head judgement:
      //
      // +0x0C = holdHeadJudge
      // +0x10 = actual accepted press sector
      //
      if (oldHoldHeadJudge[i] == JUDGE_NONE &&
          event.holdHeadJudge != JUDGE_NONE) {
        //
        // Prefer holdPressedSector rather than currentSector.
        // The game explicitly records the accepted head time here.
        //
        const int32_t error = static_cast<int32_t>(event.holdPressedSector) -
                              static_cast<int32_t>(event.sector);

        RecordJudge(static_cast<uint32_t>(i), EVENT_LONG, event.holdHeadJudge,
                  error, GetPanel(event), false);
      }

      //
      // LONG tail judgement:
      //
      // +0x02 becomes nonzero when the tail is resolved.
      //
      if (oldJudge[i] == JUDGE_NONE && event.judge != JUDGE_NONE) {
        const uint32_t targetSector = event.sector + GetHoldDuration(event);

        //
        // There is no separate tail-judged-sector field in the
        // recovered SequenceEvent. Therefore currentSector is the
        // sector at which the game resolved the tail.
        //
        const int32_t error = static_cast<int32_t>(currentSector) -
                              static_cast<int32_t>(targetSector);

        RecordJudge(static_cast<uint32_t>(i), EVENT_LONG, event.judge, error,
                  GetPanel(event), true);
      }

      break;
    }

    default:
      //
      // END / MEASURE / HAKU / TEMPO are timeline events and do not
      // contribute judgement results.
      //
      break;
    }
  }

  events = currentEvents;
}
void Analyzer::RecordJudge(uint32_t index, SequenceEventType type,
                           JudgeResult result, int32_t errorSector,
                           uint32_t panel, bool release) {
  stats.Add(index, type, result, errorSector, panel, release);

  recentJudges.push_back({
      index,
      type,
      result,
      errorSector,
      panel,
      release,
  });
}
void Analyzer::BuildRegions()
{
    regionStarts.clear();

    for (const auto &event : events)
    {
        if (event.type == EVENT_MEASURE)
            regionStarts.push_back(event.sector);
    }

    //
    // Ensure the song always has a region starting from zero.
    //
    if (regionStarts.empty() ||
        regionStarts.front() != 0)
    {
        regionStarts.insert(
            regionStarts.begin(),
            0);
    }

    regionTiming.resize(regionStarts.size());
}


size_t Analyzer::FindRegion(uint32_t sector) const
{
    auto it =
        std::upper_bound(
            regionStarts.begin(),
            regionStarts.end(),
            sector);

    if (it == regionStarts.begin())
        return 0;

    return static_cast<size_t>(
        std::distance(
            regionStarts.begin(),
            it) - 1);
}
void Analyzer::RecordPressTiming(
    uint32_t eventIndex,
    int32_t errorSector)
{
    if (eventIndex >= events.size())
        return;

    const auto &event =
        events[eventIndex];

    //
    // Only PLAY and LONG head represent physical press timing.
    //
    if (event.type != EVENT_PLAY &&
        event.type != EVENT_LONG)
    {
        return;
    }

    songTiming.Add(errorSector);

    const size_t region =
        FindRegion(event.sector);

    if (region < regionTiming.size())
        regionTiming[region].Add(errorSector);
}


double Analyzer::GetCurrentRegionAverageTimingMs(
    uint32_t currentSector) const
{
    if (regionTiming.empty())
        return 0.0;

    const size_t region =
        FindRegion(currentSector);

    if (region >= regionTiming.size())
        return 0.0;

    return regionTiming[region]
        .AverageMilliseconds();
}

} // namespace jbt
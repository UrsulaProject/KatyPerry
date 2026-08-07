#ifndef JUBEAT_ANALYZER_H
#define JUBEAT_ANALYZER_H
#include <vector>
namespace jbt {
enum SequenceEventType : uint16_t {
  EVENT_NONE = 0x0,
  EVENT_PLAY = 0x1,
  EVENT_END = 0x2,
  EVENT_MEASURE = 0x3,
  EVENT_HAKU = 0x4,
  EVENT_TEMPO = 0x5,
  EVENT_LONG = 0x6,
};
enum JudgeResult : uint16_t {
  JUDGE_NONE = 0,
  JUDGE_MISS = 1,
  JUDGE_POOR = 2,
  JUDGE_GOOD = 3,
  JUDGE_GREAT = 4,
  JUDGE_PERFECT = 5,
};
struct SequenceHoldEvent {
  uint32_t panel : 4;
  uint32_t reserved : 2;
  uint32_t holdType : 2;
  uint32_t duration : 24;
};
struct SequenceNoteEvent {
  uint32_t panel : 4;
  uint32_t padding : 28;
};
union SequenceEventUnion {
  uint32_t raw;
  SequenceNoteEvent play;
  SequenceHoldEvent hold;
  uint32_t tempo;
};

struct SequenceEvent {
  SequenceEventType type;
  JudgeResult judge;
  uint32_t sector; // 300Hz absolute timeline position
  union SequenceEventUnion eve;
  JudgeResult holdHeadJudge; // +0x0C
  uint16_t padding;          // +0x0E
  uint32_t holdPressedSector;
};
struct JudgeRecord {
  uint32_t index;
  SequenceEventType type;
  JudgeResult result;
  // Positive = late, negative = early.
  int32_t errorSector;
  uint32_t panel;
  // Only meaningful for EVENT_LONG.
  // false = head; true = tail.
  bool release;
};

struct Statistics {
  uint32_t eventCount[16]{};

  uint32_t perfect = 0;
  uint32_t great = 0;
  uint32_t good = 0;
  uint32_t poor = 0;
  uint32_t miss = 0;

  int64_t totalErrorSector = 0;

  int32_t maxEarly = 0;

  int32_t maxLate = 0;

  std::vector<JudgeRecord> records;
  void Add(uint32_t index, SequenceEventType type, JudgeResult result,
           int32_t errorSector, uint32_t panel, bool release);
  [[nodiscard]]
  uint32_t JudgeCount() const;

  [[nodiscard]]
  double AverageErrorMs() const;
};
struct TimingStatistics
{
    int64_t totalErrorSector = 0;
    uint32_t count = 0;

    void Add(int32_t errorSector)
    {
        totalErrorSector += errorSector;
        ++count;
    }

    [[nodiscard]]
    double AverageMilliseconds() const
    {
        if (!count)
            return 0.0;

        return
            static_cast<double>(totalErrorSector) /
            static_cast<double>(count) *
            1000.0 / 300.0;
    }
};

class Analyzer {
public:
  Analyzer() = delete;
  Analyzer(std::vector<SequenceEvent> events);
  void BeforeJudge(const std::vector<SequenceEvent> &currentEvents);
  void AfterJudge(const std::vector<SequenceEvent> &currentEvents,
                  uint32_t currentSector);
  [[nodiscard]]
  const std::vector<JudgeRecord> &GetRecentJudges() const {
    return recentJudges;
  }

  [[nodiscard]]
  const Statistics &GetStatistics() const {
    return stats;
  }

  [[nodiscard]]
  const std::vector<SequenceEvent> &GetEvents() const {
    return events;
  }
  
    //
    // Called only after a real physical press has been matched to a judge.
    //
    void RecordPressTiming(
        uint32_t eventIndex,
        int32_t errorSector);

    [[nodiscard]]
    double GetSongAverageTimingMs() const
    {
        return songTiming.AverageMilliseconds();
    }

    [[nodiscard]]
    double GetCurrentRegionAverageTimingMs(
        uint32_t currentSector) const;

private:
    size_t FindRegion(uint32_t sector) const;
    void BuildRegions();
  void RecordJudge(uint32_t index, SequenceEventType type, JudgeResult result,
                   int32_t errorSector, uint32_t panel, bool release);
  static uint32_t GetPanel(const SequenceEvent &event);
  static uint32_t GetHoldDuration(const SequenceEvent &event);
  void CountEvents();
  Statistics stats;
  std::vector<SequenceEvent> events;
  std::vector<JudgeResult> oldJudge;
  std::vector<JudgeResult> oldHoldHeadJudge;
  std::vector<JudgeRecord> recentJudges;
      TimingStatistics songTiming;
    std::vector<TimingStatistics> regionTiming;
    std::vector<uint32_t> regionStarts;
};
} // namespace jbt
#endif
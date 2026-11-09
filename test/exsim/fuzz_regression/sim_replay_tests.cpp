// Replays the committed corpora of the journal and scenario DSL fuzz targets.
#include "corpus_replay.hpp"
#include "test/fuzz/targets/journal_reader.hpp"
#include "test/fuzz/targets/scenario_dsl.hpp"

FUZZ_REPLAY_TEST(journal_reader)
FUZZ_REPLAY_TEST(scenario_dsl)

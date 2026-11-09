// Replays the committed corpora of the FIX 4.4 fuzz targets.
#include "corpus_replay.hpp"
#include "test/fuzz/targets/fix44_message.hpp"
#include "test/fuzz/targets/fix44_stream.hpp"

FUZZ_REPLAY_TEST(fix44_message)
FUZZ_REPLAY_TEST(fix44_stream)

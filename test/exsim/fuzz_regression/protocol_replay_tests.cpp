// Replays the committed corpora of the binary protocol fuzz targets.
#include "corpus_replay.hpp"
#include "test/fuzz/targets/itch50.hpp"
#include "test/fuzz/targets/moldudp64.hpp"
#include "test/fuzz/targets/ouch42_inbound.hpp"
#include "test/fuzz/targets/ouch42_outbound.hpp"
#include "test/fuzz/targets/sbe_order_entry.hpp"
#include "test/fuzz/targets/soupbintcp.hpp"
#include "test/fuzz/targets/wire_text_fields.hpp"

FUZZ_REPLAY_TEST(ouch42_inbound)
FUZZ_REPLAY_TEST(ouch42_outbound)
FUZZ_REPLAY_TEST(itch50)
FUZZ_REPLAY_TEST(soupbintcp)
FUZZ_REPLAY_TEST(moldudp64)
FUZZ_REPLAY_TEST(wire_text_fields)
FUZZ_REPLAY_TEST(sbe_order_entry)

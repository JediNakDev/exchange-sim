// Replays the committed corpora of the text, configuration and buffer fuzz targets.
#include "corpus_replay.hpp"
#include "fuzz/targets/admin_command.hpp"
#include "fuzz/targets/bot_options.hpp"
#include "fuzz/targets/price_text.hpp"
#include "fuzz/targets/server_config.hpp"
#include "fuzz/targets/stream_buffers.hpp"

FUZZ_REPLAY_TEST(stream_buffers)
FUZZ_REPLAY_TEST(admin_command)
FUZZ_REPLAY_TEST(server_config)
FUZZ_REPLAY_TEST(price_text)
FUZZ_REPLAY_TEST(bot_options)

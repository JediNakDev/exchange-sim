#ifndef EXSIM_RESEARCH_RUN_VERIFICATION_HPP
#define EXSIM_RESEARCH_RUN_VERIFICATION_HPP

// Offline verification of a finished run directory. The runner is
// deterministic, so the artifacts on disk must be exactly what this build
// produces again from the run's own experiment.conf, master seed, parent run
// id and recorded build labels. Re-executing the run is the only check that
// also catches an edited summary.json: the digests inside it can be edited
// together with the values they would protect.

#include "exsim/research/run_compare.hpp"

#include <filesystem>
#include <string>

namespace exsim::research {

struct RunReproduction {
    bool reproduced = false;
    std::string detail;  // empty when reproduced; otherwise what differs or why it cannot be checked
};

// Re-executes the run in `run_directory` into a scratch directory under the
// system temporary directory (removed afterwards) and compares every
// deterministic artifact (all of kRunArtifactFileNames except status.json)
// byte for byte. A cancelled run is reported as not reproduced, because when
// its cancellation arrived is not part of the recorded inputs. Throws
// ArtifactError when the directory cannot be read or the scratch run cannot
// be written.
[[nodiscard]] RunReproduction reproduce_run(const std::filesystem::path& run_directory);

// Runs reproduce_run for every run in `set` that is completed with a verified
// replay (the only runs compare would pair) and records the result in
// LoadedRun::reproduction.
void check_reproduction(RunSet& set);

}  // namespace exsim::research

#endif

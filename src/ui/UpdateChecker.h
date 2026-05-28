#pragma once
#include <string>

namespace materializr {

// Tiny GitHub-releases checker. Hits the public releases API for a given owner
// /repo, pulls the latest tag, and exposes it for comparison against the
// running build's MATERIALIZR_VERSION. No persistent state; one shot per call.
class UpdateChecker {
public:
    struct Result {
        bool ok = false;            // network call succeeded and a tag was parsed
        bool updateAvailable = false;
        std::string current;        // version baked into this binary
        std::string latest;         // tag from the GitHub release (sans leading "v")
        std::string releasePageUrl; // human-facing page for downloading the new build
        std::string errorMessage;   // populated when ok == false
    };

    // Blocks for the duration of the HTTPS request (typically <1s). Designed
    // to be called in response to a user clicking "Check for Updates".
    static Result check(const std::string& githubOwner,
                        const std::string& githubRepo);

    // Returns -1 if a < b, 0 if equal, +1 if a > b. Both inputs may carry a
    // leading "v". Non-numeric components are ignored.
    static int compareVersions(const std::string& a, const std::string& b);
};

} // namespace materializr

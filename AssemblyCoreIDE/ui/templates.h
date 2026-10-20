#pragma once
// Text for new files and projects.
#include <string>

namespace acide {

// A small two-domain program that builds without findings: Peri reads six bytes and publishes them as service SVC, Main waits for the
// result, checks the service id and copies the payload.
std::string newProgramText(const std::string& projectName);

}  // namespace acide

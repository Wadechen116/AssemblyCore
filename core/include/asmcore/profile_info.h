#pragma once
// What every profile key means, for people: the group it belongs to, its range, a one-line explanation -- and which parts of a profile are
// still PLANNING values nobody has confirmed against the hardware (the "gap list"). The form editor of the IDE is generated from this table,
// and a test keeps it in step with the loader (every key Profile::load accepts is described here, and the other way round).
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {

struct ProfileKeyInfo {
    std::string name;
    std::string group;          // profileGroups() name
    enum Type { Int, Bool, Text } type = Int;
    long long lo = 0, hi = 0;   // Int: the range the loader accepts
    std::string description;
    std::string defaultText;    // the value of a default Profile, as written in a profile file
};
const std::vector<ProfileKeyInfo>& profileKeys();
const ProfileKeyInfo* profileKey(const std::string& name);

struct ProfileGroup {
    std::string name, title;
    std::string why;            // for the confirmable groups: why the value matters and who knows it
    bool confirmable = false;   // can be marked as confirmed (`confirmed = ...` in the profile)
};
const std::vector<ProfileGroup>& profileGroups();

// One thing a profile still leaves open. `confirmed` groups are not listed.
struct ProfileGap {
    std::string group, title, why;
    std::vector<std::string> keys;
};
std::vector<ProfileGap> profileGaps(const Profile& p);

// Text editing of a profile file that keeps everything else (comments, order, spelling) as it was.
// Set: the last `key = ...` line gets the new value (its trailing comment stays); a missing key is appended.
std::string profileUpdateKey(const std::string& text, const std::string& key, const std::string& value);
// Remove every `key = ...` line (the key then has its default again).
std::string profileRemoveKey(const std::string& text, const std::string& key);
// The value written for `key` (the last one), or "" with found = false.
std::string profileGetKey(const std::string& text, const std::string& key, bool& found);

}  // namespace asmcore

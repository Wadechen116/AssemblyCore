#pragma once
// The readiness gaps (asmcore/readiness.h) in the words of the interface, so that they can be shown in the language of the user. The core says WHAT is
// missing (ReadinessItem::code, id, value); the sentences are made here. The titles and explanations of the profile groups are the core's own texts,
// looked up as translations ("ProfileGroup", "ProfileGroupWhy"): a test keeps this file's copies in step with the core.
#include <QString>

#include "asmcore/asmcore.h"

namespace acide {

struct ReadinessWords { QString title, why, fix; };
ReadinessWords readinessWords(const asmcore::ReadinessItem& item);

QString profileGroupTitle(const std::string& coreTitle);      // the title of a profile group, translated (the text itself when there is no translation)
QString profileGroupWhy(const std::string& coreWhy);

// The texts this file offers for translation, as (context, text) pairs: for the test that compares them with what the core can say.
const char* const* readinessTextsForTranslation(int& count);
// The parameter keys that have a short title here.
bool readinessHasParamTitle(const std::string& key);

}  // namespace acide

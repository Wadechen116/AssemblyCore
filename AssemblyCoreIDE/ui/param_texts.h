#pragma once
// What each hardware parameter of the simulator is for, in the language of the interface -- the comment a .simparams file carries above every
// parameter. The translation is looked up by the parameter's KEY (context "SimParamPurpose"); where there is none (English), the catalog's own help
// text is used. A test checks that every parameter of the catalog is offered for translation here.
#include <QString>

#include "asmcore/sim_params.h"

namespace acide {

QString simParamPurpose(const asmcore::ParamDef& d);
// The words the IDE writes .simparams files with (header + purpose and facts of each parameter), in the language in use now.
asmcore::SimParams::FileWords simParamsFileWordsForUi();
// The keys offered for translation (for the test).
const char* const* simParamKeysForTranslation(int& count);

}  // namespace acide

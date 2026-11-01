// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// The shipped model. models/opensection.osm is embedded here by tools/learn/embed.py.
#include "opensection/Model.h"

namespace opensection {

static const char* const kBuiltin[] = {      // several pieces: MSVC limits one literal to 16 KB
#include "BuiltinModel.inc"
};

Model Model::builtin()
{
    std::string text, err;
    for (const char* piece : kBuiltin) text += piece;
    Model m = Model::fromText(text, &err);
    return err.empty() ? m : Model::flat();
}

} // namespace opensection

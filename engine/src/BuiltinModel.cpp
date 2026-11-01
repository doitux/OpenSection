// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// The shipped model. models/opensection.osm is embedded here by tools/learn/embed.py.
#include "opensection/Model.h"

namespace opensection {

static const char* kBuiltin =
#include "BuiltinModel.inc"
    ;

Model Model::builtin()
{
    std::string err;
    Model m = Model::fromText(kBuiltin, &err);
    return err.empty() ? m : Model::flat();
}

} // namespace opensection

#pragma once

#include <slang-com-ptr.h>
#include <slang.h>

#include <vector>

#include "slangmake_runtime_internal.h"

namespace slangmake
{
/**
 * Translate a slangmake target into the matching Slang enum value.
 *
 * @param t target to translate
 * @return  the corresponding SlangCompileTarget (e.g. SLANG_SPIRV)
 */
SlangCompileTarget toSlangCompileTarget(Target t);
} // namespace slangmake

namespace slangmake::detail
{

/**
 * Walk Slang's reflection API for one linked program and pack the result into
 * the custom binary layout used by ReflectionView.
 *
 * @param gs          owning global session (for hashing helpers)
 * @param program     linked component from which layout() is queried
 * @param module      front-end module to walk for the decl tree; may be null
 * @param targetIndex target index inside the program (typically 0)
 * @return            serialised reflection section, ready to embed in a blob
 */
std::vector<uint8_t> serializeReflection(slang::IGlobalSession* gs, slang::IComponentType* program,
                                         slang::IModule* module, int targetIndex);

} // namespace slangmake::detail

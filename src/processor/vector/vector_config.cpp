/**
 * @file vector_config.cpp
 * @brief Storage for the global vector configuration.
 */
#include "processor/vector/vector_config.h"

namespace Kites
{
namespace rvv
{

VectorConfig &config()
{
    static VectorConfig instance;
    return instance;
}

} // namespace rvv
} // namespace Kites

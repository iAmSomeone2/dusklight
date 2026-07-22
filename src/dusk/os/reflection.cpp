#include "reflection.h"

bool isType(void* ptr, const ReflectiveType refType) {
    if (ptr == nullptr) {
        return false;
    }
    const auto ty = static_cast<ReflectiveType*>(ptr);
    return *ty == refType;
}

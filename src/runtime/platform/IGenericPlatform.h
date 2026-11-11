// IGenericPlatform.h — platform abstraction interface (I-prefixed).

#pragma once

namespace pictor::platform
{

// Interface for platform-specific abstractions.
class IGenericPlatform
{
public:
    virtual ~IGenericPlatform() = default;
};

}  // namespace pictor::platform

#pragma once

#include <memory>

namespace Hypodermic
{

class Container;
class ContainerBuilder;

}

namespace HomeCompa::FliFaq
{

inline constexpr auto APP_ID = "flifaqer";

void DiInit(Hypodermic::ContainerBuilder& builder, std::shared_ptr<Hypodermic::Container>& container);

}

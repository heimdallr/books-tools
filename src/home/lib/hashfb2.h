#pragma once

#include "flihash.h"

#include "export/lib.h"

class QCryptographicHash;

namespace HomeCompa::FliLib
{

LIB_EXPORT void ParseFb2Hash(BookHashItem& bookHashItem, QCryptographicHash& cryptographicHash);

}

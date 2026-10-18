#pragma once
struct CompatFlags { bool MoreAccurateVMMUL = false; };
struct Compat { CompatFlags f; const CompatFlags &flags() const { return f; } };
struct CoreParam { Compat compat; };
inline CoreParam &PSP_CoreParameter() { static CoreParam p; return p; }

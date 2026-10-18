#pragma once
#include "Common/CommonTypes.h"
#include "Core/Opcode.h"
extern "C" u8 *g_mem;
namespace Memory {
inline u8 *P(u32 a) { return g_mem + (a & 0x3FFFFFFFu); }
inline bool IsValidAddress(u32 a) { return true; }
inline bool IsValid4AlignedAddress(u32 a) { return (a & 3) == 0; }
inline bool IsValidRange(u32 a, u32 s) { return true; }
inline u32 ReadUnchecked_U32(u32 a) { u32 v; __builtin_memcpy(&v, P(a), 4); return v; }
inline float ReadUnchecked_Float(u32 a) { float v; __builtin_memcpy(&v, P(a), 4); return v; }
inline void WriteUnchecked_U32(u32 v, u32 a) { __builtin_memcpy(P(a), &v, 4); }
inline void WriteUnchecked_Float(float v, u32 a) { __builtin_memcpy(P(a), &v, 4); }
inline u8 *GetPointerUnchecked(u32 a) { return P(a); }
inline u8 *GetPointerWriteUnchecked(u32 a) { return P(a); }
inline const u8 *GetPointer(u32 a) { return P(a); }
inline u8 *GetPointerWrite(u32 a) { return P(a); }
inline u32 Read_U32(u32 a) { return ReadUnchecked_U32(a); }
inline void Write_U32(u32 v, u32 a) { WriteUnchecked_U32(v, a); }
}
enum class MemoryExceptionType { READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK };
inline void Core_MemoryException(u32, u32, u32, MemoryExceptionType, const char * = "") {}

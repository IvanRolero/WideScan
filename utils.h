#ifndef _UTILS_H_INCLUDED_
#define _UTILS_H_INCLUDED_ 1

#define INCHDIV1200_TO_MM(x) \
      ((int)(x*25.4/1200))

#define MM_TO_INCHDIV1200(x) \
      ((int)(x*1200/25.4))

#define GET_BYTE(p,i) \
    ((unsigned char) p[i])

#define GET_CHAR(p,i) \
    ((char) p[i])

#define GET_WORD(p,i) \
    (((unsigned int) p[i] << 8) + p[i+1])

#define GET_DWORD(p,i) \
    (((unsigned long) p[i]   << 24) + \
     ((unsigned long) p[i+1] << 16) + \
     ((unsigned long) p[i+2] << 8)  + \
     p[i+3])

#define PUT_WORD(p,i,v) \
   p[i]   = (unsigned char) (((v) >> 8) & 0xff); \
   p[i+1] = (unsigned char) ((v)        & 0xff)

template <typename T>
T MySwap(T val) {
    static_assert(sizeof(T) == 0, "MySwap not implemented for this type");
    return val;
}

template <>
inline unsigned short MySwap(unsigned short val) {
    return (val >> 8) | (val << 8);
}

template <>
inline unsigned long MySwap(unsigned long val) {
    return ((val >> 24) & 0x000000FF) |
           ((val >> 8)  & 0x0000FF00) |
           ((val << 8)  & 0x00FF0000) |
           ((val << 24) & 0xFF000000);
}

#endif
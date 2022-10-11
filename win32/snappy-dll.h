#ifndef UTIL_SNAPPY_SNAPPY_DLL_H_
#define UTIL_SNAPPY_SNAPPY_DLL_H_

#if defined(SNAPPY_DLL) && !defined(SNAPPY_STATIC)
#ifdef BUILDING_SNAPPY
#define __SNAPPY_API __declspec(dllexport)
#else
#define __SNAPPY_API __declspec(dllimport)
#endif
#else
#define __SNAPPY_API
#endif

#endif  // UTIL_SNAPPY_SNAPPY_DLL_H_

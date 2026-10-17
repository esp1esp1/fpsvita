/*
 * scepafre_shim.cpp - out-of-class statics that the ScePaf-RE headers
 * declare but do not define.
 *
 * Everything else the shell plugin calls is a real ScePaf export, imported
 * through the stubs generated from paf_stubs/ScePafRE.yml under its
 * ScePaf-RE name.
 */

#include <paf.h>

namespace paf {

template<>
char basic_string<char, char_traits<char>, allocator<char>>::empty_string = '\0';

template<>
wchar_t basic_string<wchar_t, char_traits<wchar_t>, allocator<wchar_t>>::empty_string = L'\0';

} /* namespace paf */

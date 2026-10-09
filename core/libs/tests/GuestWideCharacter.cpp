#include "SceTypes.hpp"
#include <cstdint>

extern "C" {
std::uintptr_t APS5_VABI wctype_nid_postfix(const char*);
int APS5_VABI iswctype_nid_postfix(std::uint32_t, std::uintptr_t);
int APS5_VABI iswalnum_nid_postfix(std::uint32_t);
int APS5_VABI iswalpha_nid_postfix(std::uint32_t);
int APS5_VABI iswblank_nid_postfix(std::uint32_t);
int APS5_VABI iswcntrl_nid_postfix(std::uint32_t);
int APS5_VABI iswdigit_nid_postfix(std::uint32_t);
int APS5_VABI iswgraph_nid_postfix(std::uint32_t);
int APS5_VABI iswlower_nid_postfix(std::uint32_t);
int APS5_VABI iswprint_nid_postfix(std::uint32_t);
int APS5_VABI iswpunct_nid_postfix(std::uint32_t);
int APS5_VABI iswspace_nid_postfix(std::uint32_t);
int APS5_VABI iswupper_nid_postfix(std::uint32_t);
int APS5_VABI iswxdigit_nid_postfix(std::uint32_t);
std::uint32_t APS5_VABI towupper_nid_postfix(std::uint32_t);
std::uint32_t APS5_VABI towlower_nid_postfix(std::uint32_t);
std::uint32_t APS5_VABI btowc_nid_postfix(int);
}

int main() {
    bool valid = true;
    valid &= iswalnum_nid_postfix('A') && iswalnum_nid_postfix('7') && !iswalnum_nid_postfix('!');
    valid &= iswalpha_nid_postfix('z') && !iswalpha_nid_postfix('4');
    valid &= iswblank_nid_postfix(' ') && iswblank_nid_postfix('\t') && !iswblank_nid_postfix('\n');
    valid &= iswcntrl_nid_postfix('\n') && iswcntrl_nid_postfix(0x7f) && !iswcntrl_nid_postfix(' ');
    valid &= iswdigit_nid_postfix('0') && !iswdigit_nid_postfix('a');
    valid &= iswgraph_nid_postfix('!') && !iswgraph_nid_postfix(' ');
    valid &= iswlower_nid_postfix('a') && !iswlower_nid_postfix('A');
    valid &= iswprint_nid_postfix(' ') && !iswprint_nid_postfix('\n');
    valid &= iswpunct_nid_postfix('?') && !iswpunct_nid_postfix('Q');
    valid &= iswspace_nid_postfix('\r') && !iswspace_nid_postfix('x');
    valid &= iswupper_nid_postfix('Z') && !iswupper_nid_postfix('z');
    valid &= iswxdigit_nid_postfix('f') && !iswxdigit_nid_postfix('g');
    valid &= towupper_nid_postfix('q') == 'Q' && towupper_nid_postfix('?') == '?';
    valid &= towlower_nid_postfix('Q') == 'q' && towlower_nid_postfix('?') == '?';
    valid &= btowc_nid_postfix(0xff) == 0xff && btowc_nid_postfix(-1) == UINT32_MAX;
    const auto alpha = wctype_nid_postfix("alpha");
    valid &= alpha != 0 && iswctype_nid_postfix('R', alpha) && !iswctype_nid_postfix('8', alpha);
    valid &= wctype_nid_postfix("unknown") == 0 && !iswctype_nid_postfix('A', 0);
    return valid ? 0 : 1;
}

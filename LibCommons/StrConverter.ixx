module;
#include <Windows.h>

export module commons.strconverter;

import std;

namespace LibCommons
{
export class StrConverter
{
public:
    static std::string ToAnsi(const std::wstring& rfUniCode)
    {
        // Convert a Unicode string to an ASCII string
        const int slength = static_cast<int>(rfUniCode.length() + 1);

        // Get Length of the multi byte buffer
        const int len = ::WideCharToMultiByte(CP_ACP, 0, rfUniCode.c_str(), slength, 0, 0, 0, 0);

        // Change Unicode (UTF-16) to multi byte buffer
        std::string ansiCode(len - 1, '\0');
        ::WideCharToMultiByte(CP_ACP, 0, rfUniCode.c_str(), slength, &ansiCode[0], len, 0, 0);

        return ansiCode;
    }

    static std::wstring ToUnicode(const std::string& rfAnsi)
    {
        const int slength = static_cast<int>(rfAnsi.length() + 1);

        // Get Length of the wide char buffer
        const int len = ::MultiByteToWideChar(CP_ACP, 0, rfAnsi.c_str(), slength, 0, 0);

        // Change multi byte buffer to Unicode (UTF-16) 
        std::wstring uniCode(len - 1, L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, rfAnsi.c_str(), slength, &uniCode[0], len);

        return uniCode;
    }
};
}

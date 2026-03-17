import json

def generate_dictionary_files(valueset_bundles):
    # Core mappings we want to intern (Index 0 is strictly reserved for NULL/Empty)
    code_map = {0: ("", "FF_CODE_NULL")}
    current_index = 1
    seen_codes = set()

    for vs in valueset_bundles:
        includes = vs.get('compose', {}).get('include', [])
        for inc in includes:
            system = inc.get('system', '')
            
            # 1. Add the System URI
            if system and system not in seen_codes:
                enum_name = f"FF_URI_{system.split('/')[-1].upper().replace('.','_').replace('-','_')}"
                code_map[current_index] = (system, enum_name)
                seen_codes.add(system)
                current_index += 1

            # 2. Add the Concepts/Codes
            for concept in inc.get('concept', []):
                code = concept.get('code', '')
                if code and code not in seen_codes:
                    clean_name = ''.join(e for e in code if e.isalnum()).upper()
                    # Prevent starting with a number (invalid C++)
                    if clean_name and clean_name[0].isdigit():
                        clean_name = "NUM_" + clean_name
                    enum_name = f"FF_CODE_{clean_name}"
                    
                    code_map[current_index] = (code, enum_name)
                    seen_codes.add(code)
                    current_index += 1

    # --- Generate Header (.hpp) ---
    hpp = "// MARK: - FastFHIR Global Dictionary\n#pragma once\n#include <stdint.h>\n#include <string>\n\n"
    hpp += "constexpr uint32_t FF_CUSTOM_STRING_FLAG = 0x80000000;\n\n"
    hpp += "enum FF_Code : uint32_t {\n"
    for idx in range(current_index):
        enum_name = code_map[idx][1]
        hpp += f"    {enum_name:<30} = {idx},\n"
    hpp += "};\n\n"
    hpp += "const char* FF_ResolveCode(uint32_t code);\n"
    hpp += "uint32_t FF_GetDictionaryCode(const std::string& str);\n"

    # --- Generate Implementation (.cpp) ---
    cpp = "// MARK: - FastFHIR Global Dictionary Implementation\n"
    cpp += "#include \"FF_Dictionary.hpp\"\n#include <unordered_map>\n\n"
    
    # Forward Lookup Array
    cpp += "const char* FF_ResolveCode(uint32_t code) {\n"
    cpp += "    static constexpr const char* DICT[] = {\n"
    for idx in range(current_index):
        raw_str = code_map[idx][0]
        cpp += f"        \"{raw_str}\",\n"
    cpp += "    };\n"
    cpp += f"    if (code >= {current_index}) return \"\"; // Bounds check\n"
    cpp += "    return DICT[code];\n}\n\n"
    
    # Reverse Lookup Hash Map
    cpp += "uint32_t FF_GetDictionaryCode(const std::string& str) {\n"
    cpp += "    static const std::unordered_map<std::string, uint32_t> REVERSE_DICT = {\n"
    for idx in range(1, current_index): # Skip NULL
        raw_str = code_map[idx][0]
        enum_name = code_map[idx][1]
        cpp += f"        {{\"{raw_str}\", {enum_name}}},\n"
    cpp += "    };\n"
    cpp += "    auto it = REVERSE_DICT.find(str);\n"
    cpp += "    return it != REVERSE_DICT.end() ? it->second : FF_CODE_NULL;\n}\n"

    return hpp, cpp
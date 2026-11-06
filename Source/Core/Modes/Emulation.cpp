// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Emulation.h"
#include "Core/Terminal.h"

#include <cstring>

namespace Ubiquitty::Modes {

void setInputEncoding(Ubiquitty::Terminal* t, Ubiquitty::Unicode::Encoding enc)
{
    if (!t)
        return;
    // Reset partial multibyte + UTF-8 decoder state so a half-consumed sequence
    // can't bleed across the switch.
    Ubiquitty::Unicode::encReset(&t->parser.inputEncoding, enc);
    Ubiquitty::Unicode::utf8Reset(&t->parser.utf8);
}

Ubiquitty::Unicode::Encoding inputEncoding(const Ubiquitty::Terminal* t) { return t ? t->parser.inputEncoding.enc : Ubiquitty::Unicode::Encoding::UTF8; }

void setInvalidUTF8Policy(Ubiquitty::Terminal* t, Ubiquitty::Unicode::InvalidUTF8Policy policy)
{
    if (t)
        t->parser.invalidUTF8Policy = policy;
}

Ubiquitty::Unicode::InvalidUTF8Policy invalidUTF8Policy(const Ubiquitty::Terminal* t) { return t ? t->parser.invalidUTF8Policy : Ubiquitty::Unicode::InvalidUTF8Policy::FFFD; }

void setEmulation(Ubiquitty::Terminal* t, Ubiquitty::VT::Emulation e)
{
    if (!t)
        return;
    t->emulation = e;
    // Start the legacy substate at ground so a mid-sequence switch can't strand
    // the legacy driver. The VT DFA never reads legacyState.
    t->legacyState = 0;
    t->legacyAddrRow = 0;
}

Ubiquitty::VT::Emulation emulation(const Ubiquitty::Terminal* t) { return t ? t->emulation : Ubiquitty::VT::Emulation::VT; }

std::optional<Ubiquitty::VT::Emulation> emulationFromName(const char* name)
{
    if (!name)
        return std::nullopt;
    struct Entry {
        const char* token;
        Ubiquitty::VT::Emulation value;
    };
    static const Entry table[] = {
        { "vt", Ubiquitty::VT::Emulation::VT },
        { "linux", Ubiquitty::VT::Emulation::Linux },
        { "sco", Ubiquitty::VT::Emulation::Sco },
        { "wyse", Ubiquitty::VT::Emulation::Wyse },
        { "televideo", Ubiquitty::VT::Emulation::Televideo },
        { "adm", Ubiquitty::VT::Emulation::Adm },
        { "heath", Ubiquitty::VT::Emulation::Heath },
        { "hp", Ubiquitty::VT::Emulation::Hp },
    };
    for (const Entry& e : table) {
        if (std::strcmp(name, e.token) == 0)
            return e.value;
    }
    return std::nullopt;
}

} // namespace Ubiquitty::Modes

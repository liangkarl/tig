#!/usr/bin/env bash

make -j2 CPPFLAGS='-DHAVE_NCURSESW_CURSES_H -DHAVE_READLINE' LDLIBS='-ldl -lncursesw -lreadline' || exit $?

if [[ -e "${HOME}/.local/bin/tig" ]]; then
    exit
fi

ln -sf "$(pwd)/src/tig" "${HOME}/.local/bin/tig"

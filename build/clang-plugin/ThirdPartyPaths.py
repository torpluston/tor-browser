#!/usr/bin/env python

import json


def generate(output, tpp_txt):
    """
    This file generates a ThirdPartyPaths.cpp file from the ThirdPartyPaths.txt
    file in /tools/rewriting, which is used by the Clang Plugin to help identify
    sources which should be ignored.
    """

    tpp_list = []
    with open(tpp_txt) as f:
        for line in f.readlines():
            line = line.strip()
            if line.endswith('/'):
                line = line[:-1]
            tpp_list.append(line)
    tpp_strings = ',\n  '.join([json.dumps(tpp) for tpp in tpp_list])

    output.write("""\
/* THIS FILE IS GENERATED BY ThirdPartyPaths.py - DO NOT EDIT */

#include <stdint.h>

const char* MOZ_THIRD_PARTY_PATHS[] = {
  %s
};

extern const uint32_t MOZ_THIRD_PARTY_PATHS_COUNT = %d;

""" % (tpp_strings, len(tpp_list)))

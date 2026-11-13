#!/bin/bash
grep -n -i "undefined\|check-k64\|bound to 0\|k64 undefined" /tmp/mk.log | tail -30

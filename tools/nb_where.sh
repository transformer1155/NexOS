#!/bin/bash
echo "HOME=$HOME"
ls -la "$HOME" | head -20
find / -maxdepth 4 -name "Makefile" -path "*NexOS*" 2>/dev/null | head -5

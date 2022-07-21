import os, sys, requests, re

os.chdir()

keys = []
with open('lang.strings') as f:
    for line in f:
        if m := re.match(r'\"(lng_[a-z_]+)(\#[a-z]+)?\"', line):
            keys.append(m[1])
        elif not re.match(r'^\s*$', line):
            print(f'Bad line: {line}')
            sys.exit(1)

print(f'Keys: {len(keys)}')

sys.exit()

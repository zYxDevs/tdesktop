'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''
import sys, os, re, subprocess, io

def finish(code):
    global executePath
    os.chdir(executePath)
    sys.exit(code)

if sys.platform == 'win32' and 'COMSPEC' not in os.environ:
    print('[ERROR] COMSPEC environment variable is not set.')
    finish(1)

executePath = os.getcwd()
scriptPath = os.path.dirname(os.path.realpath(__file__))

inputVersion = ''
versionOriginal = ''
versionMajor = ''
versionMinor = ''
versionPatch = ''
versionAlpha = '0'
versionBeta = False
for arg in sys.argv:
    if match := re.match(
        r'^\s*(\d+)\.(\d+)(\.(\d+)(\.(\d+|beta))?)?\s*$', arg
    ):
        inputVersion = arg
        versionOriginal = inputVersion
        versionMajor = match[1]
        versionMinor = match[2]
        versionPatch = match[4] or '0'
        versionAlphaBeta = match[5] or ''
        if len(versionAlphaBeta) > 0:
            if match[6] == 'beta':
                versionBeta = True
            else:
                versionAlpha = match[6]

if not len(versionMajor):
  print("Wrong version parameter")
  finish(1)

def checkVersionPart(part):
    cleared = int(part) % 1000 if len(part) > 0 else 0
    if str(cleared) != part:
        print(f"Bad version part: {part}")
        finish(1)

checkVersionPart(versionMajor)
checkVersionPart(versionMinor)
checkVersionPart(versionPatch)
checkVersionPart(versionAlpha)

versionFull = str(int(versionMajor) * 1000000 + int(versionMinor) * 1000 + int(versionPatch))
versionFullAlpha = '0'
if versionAlpha != '0':
  versionFullAlpha = str(int(versionFull) * 1000 + int(versionAlpha))

versionStr = f'{versionMajor}.{versionMinor}.{versionPatch}'
versionStrSmall = (
    versionStr if versionPatch != '0' else f'{versionMajor}.{versionMinor}'
)


if versionBeta:
    print(f'Setting version: {versionStr} beta')
elif versionAlpha != '0':
    print(f'Setting version: {versionStr}.{versionAlpha} closed alpha')
else:
    print(f'Setting version: {versionStr} stable')

#def replaceInFile(path, replaces):

def checkChangelog():
    global scriptPath, versionStr, versionStrSmall

    count = 0
    with io.open(f'{scriptPath}/../../changelog.txt', encoding='utf-8') as f:
        for line in f:
            if line.startswith(f'{versionStr} ') or line.startswith(
                f'{versionStrSmall} '
            ):
                count = count + 1
    if count == 0:
        print('Changelog entry not found!')
        finish(1)
    elif count != 1:
        print(f'Wrong changelog entries count found: {count}')
        finish(1)

#checkChangelog()

def replaceInFile(path, replacements):
    content = ''
    foundReplacements = {}
    updated = False
    with open(path, 'r') as f:
      for line in f:
        for replacement in replacements:
          if re.search(replacement[0], line):
            changed = re.sub(replacement[0], replacement[1], line)
            if changed != line:
              line = changed
              updated = True
            foundReplacements[replacement[0]] = True
        content = content + line
    for replacement in replacements:
        if replacement[0] not in foundReplacements:
            print('Could not find "' + replacement[0] + '" in "' + path + '".')
            finish(1)
    if updated:
      with open(path, 'w') as f:
        f.write(content)

print('Patching build/version...')
replaceInFile(
    f'{scriptPath}/version',
    [
        [r'(AppVersion\s+)\d+', r'\g<1>' + versionFull],
        [
            r'(AppVersionStrMajor\s+)\d[\d\.]*',
            r'\g<1>' + versionMajor + '.' + versionMinor,
        ],
        [r'(AppVersionStrSmall\s+)\d[\d\.]*', r'\g<1>' + versionStrSmall],
        [r'(AppVersionStr\s+)\d[\d\.]*', r'\g<1>' + versionStr],
        [r'(BetaChannel\s+)\d', r'\g<1>' + ('1' if versionBeta else '0')],
        [r'(AlphaVersion\s+)\d+', r'\g<1>' + versionFullAlpha],
        [r'(AppVersionOriginal\s+)\d[\d\.beta]*', r'\g<1>' + versionOriginal],
    ],
)


print('Patching core/version.h...')
replaceInFile(
    f'{scriptPath}/../SourceFiles/core/version.h',
    [
        [
            r'(TDESKTOP_REQUESTED_ALPHA_VERSION\s+)\(\d+ULL\)',
            r'\g<1>(' + versionFullAlpha + 'ULL)',
        ],
        [r'(AppVersion\s+=\s+)\d+', r'\g<1>' + versionFull],
        [r'(AppVersionStr\s+=\s+)[^;]+', r'\g<1>"' + versionStrSmall + '"'],
        [
            r'(AppBetaVersion\s+=\s+)[a-z]+',
            r'\g<1>' + ('true' if versionBeta else 'false'),
        ],
    ],
)


parts = [versionMajor, versionMinor, versionPatch, versionAlpha]
withcomma = ','.join(parts)
withdot = '.'.join(parts)
rcReplaces = [
  [ r'(FILEVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma ],
  [ r'(PRODUCTVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma ],
  [ r'("FileVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"' ],
  [ r'("ProductVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"' ],
]

print('Patching Telegram.rc...')
replaceInFile(f'{scriptPath}/../Resources/winrc/Telegram.rc', rcReplaces)

print('Patching Updater.rc...')
replaceInFile(f'{scriptPath}/../Resources/winrc/Updater.rc', rcReplaces)

print('Patching appxmanifest.xml...')
replaceInFile(
    f'{scriptPath}/../Resources/uwp/AppX/AppxManifest.xml',
    [
        [r'( Version=)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"'],
    ],
)

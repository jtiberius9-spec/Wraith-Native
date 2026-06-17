import sys
import os

# Derives the app version from the latest git tag and writes it to
# QtScrcpy/appversion. If there are NO tags reachable (e.g. a manual CI build
# or a fresh clone without tags), it leaves the committed appversion untouched
# instead of blanking it (which used to break the CMake project() version).

if __name__ == '__main__':
    commit = os.popen('git rev-list --tags --max-count=1').read().strip()

    tag = ''
    if commit:
        tag = os.popen('git describe --tags ' + commit).read().strip()

    version_file = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../QtScrcpy/appversion"))

    if tag:
        version = tag[1:] if tag.lower().startswith('v') else tag
        with open(version_file, 'w') as f:
            f.write(version)
        print('generate-version: wrote version from tag ->', version)
    else:
        print('generate-version: no tags found, keeping existing appversion')

    sys.exit(0)

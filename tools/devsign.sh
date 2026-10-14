#!/bin/zsh
# A stable code-signing identity for the user-space helpers, so that macOS privacy grants (TCC: Screen Recording for
# nvvdisplay) survive rebuilds. An ad-hoc signature's designated requirement is the binary's cdhash, which every
# rebuild changes, so TCC treats the rebuilt nvvdisplay as a new program. Signed with a certificate, the requirement is
# `identifier "io.github.kvarun-p.nvvdisplay" and certificate leaf = H"<certificate hash>"`, which every build signed
# with the same certificate satisfies.
#
#   tools/devsign.sh howto                 how to make the certificate (this script never touches the keychain)
#   tools/devsign.sh status                the identities codesign can use, and which one local.env picks
#   tools/devsign.sh sign FILE [IDENT]     sign FILE with NVB_SIGN_IDENTITY (else ad-hoc), identifier IDENT
#                                          (default io.github.kvarun-p.<file name>)
#   tools/devsign.sh check FILE            print FILE's designated requirement and whether TCC grants survive a rebuild
#
# The identity comes from NVB_SIGN_IDENTITY in local.env or the environment: the certificate's common name (e.g.
# "NVBringup Local Dev") or its SHA-1. `make -C display vdisplay` signs nvvdisplay with it too. The kexts and
# NVMetal.bundle stay ad-hoc: OpenCore and the auxiliary collection don't check signers, and WindowServer's library
# validation is an AMFI matter that a self-signed certificate doesn't change.
set -u
[[ -n ${2:-} ]] && set -- "$1" "${2:A}" "${@:3}"     # FILE relative to the caller, before the cd
cd "${0:A:h}/.." || exit 1
[[ -f local.env ]] && { set -a; . ./local.env; set +a; }
ID=${NVB_SIGN_IDENTITY:-}

case ${1:-} in
howto)
    cat <<'EOF'
Make a self-signed code-signing certificate once, in your login keychain (by hand: this script never does it).

A. Keychain Access (simplest)
   1. Keychain Access → Keychain Access menu → Certificate Assistant → Create a Certificate…
   2. Name: NVBringup Local Dev; Identity Type: Self Signed Root; Certificate Type: Code Signing. Create.
      (The default validity is 365 days; tick "Let me override defaults" for a longer one. A new certificate means
      granting Screen Recording once more.)
   3. Optional, to keep codesign from asking for the key each time: in Keychain Access, open the private key
      "NVBringup Local Dev" → Access Control → add /usr/bin/codesign.

B. Terminal (same result)
   d=$(mktemp -d) && cd $d
   openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout key.pem -out cert.pem \
       -subj "/CN=NVBringup Local Dev" \
       -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=critical,codeSigning" \
       -addext "basicConstraints=critical,CA:false"
   openssl pkcs12 -export -legacy -inkey key.pem -in cert.pem -out dev.p12 -passout pass:nvdev   # LibreSSL: no -legacy
   security import dev.p12 -k ~/Library/Keychains/login.keychain-db -P nvdev -T /usr/bin/codesign
   rm -P key.pem dev.p12

Then:
   echo 'NVB_SIGN_IDENTITY="NVBringup Local Dev"' >> local.env
   tools/devsign.sh status                     # the identity should be listed
   make -C display vdisplay && display/vdisplay/install.sh
   tools/devsign.sh check ~/Library/Application\ Support/NVBringup/nvvdisplay
   Grant Screen Recording once more (the requirement changed from the cdhash to the certificate); from then on a
   rebuild and reinstall keep it.

The certificate doesn't need to be trusted for this: TCC compares the signature with the requirement it stored, not
with a trust chain. If codesign refuses an untrusted identity on your macOS, mark it trusted for code signing only:
   security add-trusted-cert -r trustRoot -p codeSign -k ~/Library/Keychains/login.keychain-db cert.pem
Keep it out of the System keychain, and don't use it to sign anything you give to others.
EOF
    ;;
status)
    print "identities codesign can use (security find-identity -p codesigning):"
    security find-identity -p codesigning 2>&1 | sed 's/^/  /'
    if [[ -n $ID ]]; then
        if security find-identity -p codesigning 2>/dev/null | grep -qF -- "$ID"; then
            print "NVB_SIGN_IDENTITY=$ID: found"
        else
            print "NVB_SIGN_IDENTITY=$ID: NOT found (tools/devsign.sh howto)"; exit 1
        fi
    else
        print "NVB_SIGN_IDENTITY isn't set: builds are signed ad-hoc (tools/devsign.sh howto)"
    fi
    ;;
sign)
    f=${2:?usage: devsign.sh sign FILE [IDENT]}
    ident=${3:-io.github.kvarun-p.${f:t:r}}
    if [[ -n $ID ]]; then
        codesign --force --sign "$ID" --identifier "$ident" --timestamp=none "$f" || exit 1
    else
        codesign --force --sign - --identifier "$ident" "$f" || exit 1
    fi
    ;;
check)
    f=${2:?usage: devsign.sh check FILE}
    req=$(codesign -d -r- "$f" 2>&1 | sed -n 's/^#* *designated => //p')
    auth=$(codesign -dvv "$f" 2>&1 | sed -n 's/^Authority=//p' | head -1)
    print "$f"
    print "  signer:      ${auth:-ad-hoc}"
    print "  requirement: ${req:-none}"
    if [[ $req == *"certificate leaf"* || $req == *"anchor"* ]]; then
        print "  stable: a rebuild signed with the same certificate keeps its privacy grants"
    else
        print "  NOT stable: the requirement is the cdhash, so every rebuild loses its privacy grants"; exit 1
    fi
    ;;
*)
    sed -n 2,19p "$0"; exit 2 ;;
esac

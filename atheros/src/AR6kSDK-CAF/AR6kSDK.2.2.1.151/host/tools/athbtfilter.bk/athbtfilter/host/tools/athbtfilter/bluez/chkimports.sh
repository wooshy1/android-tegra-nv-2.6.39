#!/bin/sh

#Create a list of all exported symbols
rm merged.lst
for d in *.so
do
    ~/android-ndk-r5b/toolchains/arm-eabi-4.4.0/prebuilt/linux-x86/bin/arm-eabi-readelf -W -s $d | grep -v 'UND' | cut -b52- | sort -u >> merged.lst
done

#unique it
sort -u merged.lst > unique.lst
rm merged.lst

#Now, create a list of required syms from the lib we want
~/android-ndk-r5b/toolchains/arm-eabi-4.4.0/prebuilt/linux-x86/bin/arm-eabi-readelf -W -s $1 | grep 'UND' | cut -b52- | sort -u > required.lst

#add both files
cat unique.lst > t.tmp
cat required.lst >> t.tmp

#All lines duplicated are resolved
sort t.tmp | uniq -d > resolved.lst

#Add resolved and required
cat resolved.lst > u.tmp
cat required.lst >> u.tmp

#If a line has no duplicate, means it was never resolved
sort u.tmp | uniq -u > pending.lst

cat pending.lst

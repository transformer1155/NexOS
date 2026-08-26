import urllib.request
url = 'https://github.com/progrium/qemu/releases/download/v2.4.0/qemu-system-x86_64_2.4.0.tar.gz'
urllib.request.urlretrieve(url, '/mnt/d/MyOS/bootloader/qemu.tgz')
print('downloaded')

-------------------------------------------------------------------------------
Late Lee readme
coreboot master repository from https://github.com/coreboot/coreboot

----------------------------------------

����ϵͳ��
Ubuntu 14.04.1
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
һ��coreboot�ı���
1����װ���뻷���ر����ߣ�
m4��bison��flex��
$ sudo apt-get install m4 bison flex

��װiasl(��corebootԴ��Ŀ¼������)��
$ make iasl

----------------------------------------

2�����ã�
$ make menuconfig

��ͨ�������У�Ҫѡ��Allow building with any toolchain��������Ҫmake crossgcc���½�һ�������������
General setup  --->
[*] Allow building with any toolchain

����ʹ��ģ������QEMU��
Mainboard  --->
Mainboard vendor (Emulation)  --->
Mainboard model (QEMU x86 i440fx/piix4 (aka qemu -M pc))  ---> //ʹ��x86ƽ̨��
ROM chip size (8192 KB (8 MB))  --->  // flash��С����ʵ�����ѡ��һ�������Ϊ8MiB

Playloadѡ��SeaBIOS(��
Payload  --->  
 Add a payload (SeaBIOS)  --->
 SeaBIOS version (1.9.0)  --->

ע�����ֿ����Ѿ�������SeaBIOSԴ�룬��������qemu_x86�������ļ���config_qemu_x86��ֱ�Ӽ��ؼ���ʹ��

����Ĭ�ϡ�

----------------------------------------

3�����룺
$ make

�ڴ˹��̣���clone 3rdpartyĿ¼�ֿ⣬����������û�ѡ���payload��Ȼ��һ����롣
ע��������Ҫʹ��vboot���粻����makeʱclone��Ҳ���Խ�ѹvboot.tar.bz2

�������ɣ�
build/coreboot.rom

+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

����x86 QEMUģ�⻷������
1����װqemuģ������ֻ���x86����
$ sudo apt-get install qemu-system-x86
ע��qemu��������ƽ̨�ģ����£�
* qemu-system-arm
* qemu-system-mips
* qemu-system-misc
* qemu-system-ppc
* qemu-system-sparc
* qemu-system-x86

��ʹ��ϵͳ������BIOS���������£�
$ qemu-system-i386 -nographic -bios coreboot/build/coreboot.rom

ע������û��ϵͳ�������󼴡�����������Ҫ�ر��ն˲ſ��˳���

ʹ��ϵͳ������BIOS���������£�
$ qemu-system-i386 -nographic -bios coreboot/build/coreboot.rom -hda disk.img

ע��disk.img�����ǿ������Ĵ��̾���
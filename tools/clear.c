/*
 * Minimal `clear` for Term49.
 *
 * BB10 has no ncurses binary and no /usr/bin/clear, and a shell script
 * would be installed without the execute bit (the packager only marks
 * Qnx/Elf assets executable). So this is a real ELF that just emits the
 * xterm sequences: home, erase screen, erase saved lines (CSI 3J).
 */
#include <stdio.h>

int main(void) {
	fputs("\033[H\033[2J\033[3J", stdout);
	fflush(stdout);
	return 0;
}

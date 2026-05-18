#include <wchar.h>

int main(void) {
	int i;
	char s[16];
	int ret;

	//ret = swscanf(L"56789 012356a72", L"%2d%*d %[0123456789]", &i, s);
	ret = swscanf(L"56789", L"%2d%*d %[0123456789]", &i, s);
	return ret;
}

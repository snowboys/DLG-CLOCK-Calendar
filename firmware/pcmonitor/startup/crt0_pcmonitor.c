/* 最小 crt0: SDK GCC startup 完成数据段初始化后跳到这里, 再进入 main */
extern int main(void);

void _start(void) __attribute__((noreturn, section(".text.startup._start")));
void _start(void)
{
    main();
    while (1) { }
}

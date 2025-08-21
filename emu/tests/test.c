int add(int a, int b);

// The main function put on 0x0 intentionally
int main()
{
    volatile char *tohost_addr;

    int c = add(3, 4);

    /* Because the binary will be run on baremetal environment
     * which doesn't support C-runtime. Stop the program by writing
     * to this specicial address */
    tohost_addr = (volatile char *) (0x1000 - 4);
    *tohost_addr = 0xff;
    return 0;
}

int add(int a, int b)
{
    return a + b;
}

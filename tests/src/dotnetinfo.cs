// .NET (Wine Mono) smoke test: runtime version, a little LINQ and a Windows Forms-free API call.
using System;
using System.Linq;
class Program
{
    static int Main()
    {
        int sum = Enumerable.Range(1, 100).Where(n => n % 2 == 0).Sum();
        Console.WriteLine("runtime: " + Environment.Version + ", 64-bit process: " + Environment.Is64BitProcess);
        Console.WriteLine("sum of evens 1..100: " + sum + " (expect 2550)");
        return sum == 2550 ? 0 : 1;
    }
}

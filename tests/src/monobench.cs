// Mono JIT workload shaped like a Unity game's main thread, for A/B testing FEX builds on Mono-JIT'd x86-64 code
// (Wine Mono here, Unity's Mono in games). Build: mcs -optimize+ -platform:x64 -r:System.dll monobench.cs
//   monobench.exe [scale]   prints ns per operation for each part, best of 5
using System;
using System.Collections.Generic;
using System.Diagnostics;

struct Vec3
{
    public float x, y, z;
    public Vec3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
    public static Vec3 operator +(Vec3 a, Vec3 b) { return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
    public static Vec3 operator *(Vec3 a, float s) { return new Vec3(a.x * s, a.y * s, a.z * s); }
    public float Dot(Vec3 b) { return x * b.x + y * b.y + z * b.z; }
}

interface IDamageable { int Hit(int amount); }

abstract class Component
{
    public Vec3 position, velocity;
    public Component target;   // reference stores go through the GC write barrier
    public int state;
    public abstract void Update(float dt);
}

// Several component classes, so the virtual Update calls are polymorphic like a scene's scripts.
class Mover : Component { public override void Update(float dt) { position = position + velocity * dt; state++; } }
class Spinner : Component { public override void Update(float dt) { velocity = new Vec3(velocity.z, velocity.x, -velocity.y); state += 2; } }
class Seeker : Component
{
    public override void Update(float dt)
    {
        if (target != null) { var d = new Vec3(target.position.x - position.x, target.position.y - position.y, target.position.z - position.z); velocity = velocity + d * (0.01f * dt); }
        state++;
    }
}
class Health : Component, IDamageable
{
    int hp = 100;
    public int Hit(int amount) { hp -= amount; if (hp <= 0) hp = 100; return hp; }
    public override void Update(float dt) { if ((state++ & 7) == 0) Hit(1); }
}
class Animator2 : Component
{
    float[] curve = new float[16];
    public override void Update(float dt) { int i = state++ & 15; curve[i] = curve[(i + 1) & 15] * 0.5f + dt; position.y = curve[i]; }
}

class Program
{
    static List<Component> scene = new List<Component>();
    static Dictionary<int, Component> byId = new Dictionary<int, Component>();
    static long sink;

    static void Build(int n)
    {
        var rng = new Random(7);
        for (int i = 0; i < n; i++)
        {
            Component c;
            switch (rng.Next(5))
            {
                case 0: c = new Mover(); break;
                case 1: c = new Spinner(); break;
                case 2: c = new Seeker(); break;
                case 3: c = new Health(); break;
                default: c = new Animator2(); break;
            }
            c.position = new Vec3(rng.Next(100), rng.Next(100), rng.Next(100));
            c.velocity = new Vec3(1, 0.5f, 0.25f);
            scene.Add(c);
            byId[i * 7 + 3] = c;
        }
        for (int i = 0; i < n; i++) scene[i].target = scene[rng.Next(n)];
    }

    // Virtual Update over the whole scene: what MonoBehaviour updates come down to.
    static long VirtualUpdates(int reps)
    {
        for (int r = 0; r < reps; r++)
            for (int i = 0; i < scene.Count; i++) scene[i].Update(0.016f);
        return (long)reps * scene.Count;
    }

    // Interface calls through type checks.
    static long InterfaceCalls(int reps)
    {
        long s = 0;
        for (int r = 0; r < reps; r++)
            foreach (var c in scene) { var d = c as IDamageable; if (d != null) s += d.Hit(1); }
        sink += s;
        return (long)reps * scene.Count;
    }

    // Dictionary lookups by id.
    static long Lookups(int reps)
    {
        long s = 0;
        int n = scene.Count;
        for (int r = 0; r < reps; r++)
            for (int i = 0; i < n; i++) { Component c; if (byId.TryGetValue(i * 7 + 3, out c)) s += c.state; }
        sink += s;
        return (long)reps * n;
    }

    // Struct vector math over arrays (bounds checks, float ops).
    static long VectorMath(int reps)
    {
        int n = 4096;
        var a = new Vec3[n];
        for (int i = 0; i < n; i++) a[i] = new Vec3(i, i * 0.5f, 1);
        float acc = 0;
        for (int r = 0; r < reps; r++)
            for (int i = 1; i < n; i++) { a[i] = a[i] + a[i - 1] * 0.001f; acc += a[i].Dot(a[i - 1]); }
        sink += (long)acc;
        return (long)reps * (n - 1);
    }

    // Short-lived allocations and reference stores.
    static long Allocations(int reps)
    {
        var keep = new Component[256];
        for (int r = 0; r < reps; r++)
            for (int i = 0; i < 256; i++) { var m = new Mover(); m.target = scene[i]; keep[i] = m; }
        sink += keep[17].state;
        return (long)reps * 256;
    }

    // A List<T> that grows and is iterated with LINQ-free delegates, like event callbacks.
    static long Delegates(int reps)
    {
        var handlers = new List<Func<int, int>>();
        for (int i = 0; i < 32; i++) { int k = i; handlers.Add(x => x * 3 + k); }
        long s = 0;
        for (int r = 0; r < reps; r++)
            for (int i = 0; i < handlers.Count; i++) s += handlers[i]((int)s & 1023);
        sink += s;
        return (long)reps * handlers.Count;
    }

    static void Run(string name, Func<int, long> fn, int reps)
    {
        fn(Math.Max(1, reps / 10));   // JIT and warm up
        double best = double.MaxValue;
        for (int t = 0; t < 5; t++)
        {
            var sw = Stopwatch.StartNew();
            long ops = fn(reps);
            double ns = sw.Elapsed.TotalMilliseconds * 1e6 / ops;
            if (ns < best) best = ns;
        }
        Console.WriteLine("{0,-16} {1,10:F2} ns/op", name, best);
    }

    static int Main(string[] args)
    {
        double scale = args.Length > 0 ? double.Parse(args[0], System.Globalization.CultureInfo.InvariantCulture) : 1.0;
        Build(4096);
        Run("virtual_update", VirtualUpdates, (int)(400 * scale));
        Run("interface_call", InterfaceCalls, (int)(400 * scale));
        Run("dict_lookup", Lookups, (int)(200 * scale));
        Run("vector_math", VectorMath, (int)(400 * scale));
        Run("allocations", Allocations, (int)(4000 * scale));
        Run("delegates", Delegates, (int)(40000 * scale));
        Console.WriteLine("64-bit: {0}, sink {1}", Environment.Is64BitProcess, sink & 0xffff);
        return 0;
    }
}

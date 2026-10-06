//
// Program.cs - picks one sample by name and runs it.
//
//   dotnet run -- Transactions [database]
//   dotnet run                         (lists the samples)
//
using System.Reflection;

namespace FbSamples;

public static class Program
{
    public static int Main(string[] args)
    {
        var samples = typeof(Program).Assembly.GetTypes()
            .Where(t => t.Namespace == "FbSamples.Samples"
                        && t.GetMethod("Run", BindingFlags.Public | BindingFlags.Static) != null)
            .ToDictionary(t => t.Name, StringComparer.OrdinalIgnoreCase);

        if (args.Length == 0 || !samples.TryGetValue(args[0], out var type))
        {
            Console.Error.WriteLine("usage: dotnet run -- <sample> [database]");
            Console.Error.WriteLine("samples: " + string.Join(" ", samples.Keys.Order()));
            return 2;
        }
        var run = type.GetMethod("Run", BindingFlags.Public | BindingFlags.Static)!;
        return FbSample.Guard(() => run.Invoke(null, new object[] { args.Skip(1).ToArray() }));
    }
}

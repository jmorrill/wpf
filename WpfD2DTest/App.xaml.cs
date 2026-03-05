using System;
using System.Windows;

namespace WpfD2DTest;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // If the user-level env var says D2D is disabled, make sure the
        // process-level var agrees. Otherwise default to D2D enabled.
        string? userVal = Environment.GetEnvironmentVariable("WPF_USE_D2D", EnvironmentVariableTarget.User);
        if (userVal == "0")
        {
            Environment.SetEnvironmentVariable("WPF_USE_D2D", "0");
        }
        else
        {
            // Default: enable D2D
            Environment.SetEnvironmentVariable("WPF_USE_D2D", "1");
        }

        base.OnStartup(e);
    }
}


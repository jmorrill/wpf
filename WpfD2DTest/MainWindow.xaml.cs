using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;
using System.Reflection;

namespace WpfD2DTest;

// ── DXGI adapter detection ───────────────────────────────────────
static class GpuInfo
{
    [DllImport("dxgi.dll")]
    private static extern int CreateDXGIFactory1(
        ref Guid riid, out IntPtr ppFactory);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DXGI_ADAPTER_DESC
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Description;
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nuint DedicatedVideoMemory;
        public nuint DedicatedSystemMemory;
        public nuint SharedSystemMemory;
        public long AdapterLuid;
    }

    // Vtable delegates for the COM methods we need.
    // IDXGIFactory1 vtable layout (x64):
    //   [0] QueryInterface  [1] AddRef  [2] Release         (IUnknown)
    //   [3] SetPrivateData  [4] SetPrivateDataInterface
    //   [5] GetPrivateData  [6] GetParent                   (IDXGIObject)
    //   [7] EnumAdapters    [8] MakeWindowAssociation
    //   [9] GetWindowAssociation  [10] CreateSwapChain
    //   [11] CreateSoftwareAdapter                           (IDXGIFactory)
    //   [12] EnumAdapters1  [13] IsCurrent                  (IDXGIFactory1)

    private delegate int EnumAdaptersDelegate(IntPtr self, uint index, out IntPtr ppAdapter);
    private delegate int GetDescDelegate(IntPtr self, out DXGI_ADAPTER_DESC pDesc);

    // IDXGIAdapter vtable:
    //   [0-2] IUnknown  [3-6] IDXGIObject  [7] GetParent
    //   [8] GetDesc  [9] CheckInterfaceSupport

    public static (string adapterName, bool isWarp, long dedicatedMB) GetPrimaryAdapter()
    {
        IntPtr pFactory = IntPtr.Zero;
        IntPtr pAdapter = IntPtr.Zero;
        try
        {
            var iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387"); // IDXGIFactory1
            int hr = CreateDXGIFactory1(ref iid, out pFactory);
            if (hr < 0 || pFactory == IntPtr.Zero)
                return ("(factory failed)", false, 0);

            // Read factory vtable, slot 7 = EnumAdapters
            IntPtr vtFactory = Marshal.ReadIntPtr(pFactory);
            IntPtr fnEnum = Marshal.ReadIntPtr(vtFactory, 7 * IntPtr.Size);
            var enumAdapters = Marshal.GetDelegateForFunctionPointer<EnumAdaptersDelegate>(fnEnum);

            hr = enumAdapters(pFactory, 0, out pAdapter);
            if (hr < 0 || pAdapter == IntPtr.Zero)
                return ("(no adapter)", true, 0);

            // Read adapter vtable, slot 8 = GetDesc
            IntPtr vtAdapter = Marshal.ReadIntPtr(pAdapter);
            IntPtr fnGetDesc = Marshal.ReadIntPtr(vtAdapter, 8 * IntPtr.Size);
            var getDesc = Marshal.GetDelegateForFunctionPointer<GetDescDelegate>(fnGetDesc);

            hr = getDesc(pAdapter, out DXGI_ADAPTER_DESC desc);
            if (hr < 0)
                return ("(GetDesc failed)", false, 0);

            bool isWarp = desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
            long dedMB = (long)desc.DedicatedVideoMemory / (1024 * 1024);

            return (desc.Description.TrimEnd('\0'), isWarp, dedMB);
        }
        catch (Exception ex)
        {
            return ($"(error: {ex.Message})", false, 0);
        }
        finally
        {
            if (pAdapter != IntPtr.Zero) Marshal.Release(pAdapter);
            if (pFactory != IntPtr.Zero) Marshal.Release(pFactory);
        }
    }
}

public partial class MainWindow : Window
{
    // ── FPS tracking ──────────────────────────────────────────────
    private readonly DispatcherTimer _fpsTimer;
    private int _frameCount;
    private readonly Stopwatch _sw = Stopwatch.StartNew();
    private int _totalShapes;

    // ── Particle system ───────────────────────────────────────────
    private readonly List<Ellipse> _particles = new();
    private readonly List<Vector> _velocities = new();
    private readonly Random _rng = new();

    // ── Transform layer objects ───────────────────────────────────
    private readonly List<FrameworkElement> _spinners = new();
    private bool _isLoaded;

    // ── Image cache (avoid decoding the same resource multiple times) ──
    private readonly Dictionary<string, BitmapImage> _imageCache = new();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string lpModuleName);

    // ──────────────────────────────────────────────────────────────
    public MainWindow()
    {
        InitializeComponent();

        Loaded += OnLoaded;

        // FPS counter
        CompositionTarget.Rendering += OnRendering;
        _fpsTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _fpsTimer.Tick += OnFpsTick;
        _fpsTimer.Start();
    }

    // ══════════════════════════════════════════════════════════════
    //  Loaded
    // ══════════════════════════════════════════════════════════════
    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Backend detection
        bool usingD2D = Environment.GetEnvironmentVariable("WPF_USE_D2D") == "1";
        bool hasD3D11 = GetModuleHandle("d3d11.dll") != IntPtr.Zero;
        bool hasD2D1  = GetModuleHandle("d2d1.dll") != IntPtr.Zero;

        string backend = (usingD2D && hasD3D11 && hasD2D1)
            ? "D2D1 + D3D11 (active)"
            : "Legacy DX9";

        // GPU / WARP detection via DXGI
        var (gpuName, isWarp, dedMB) = GpuInfo.GetPrimaryAdapter();
        string gpuLabel = isWarp
            ? $"WARP (software)"
            : $"{gpuName} ({dedMB} MB)";

        StatusText.Text = $"Backend: {backend}  |  GPU: {gpuLabel}";
        if (isWarp)
            StatusText.Foreground = new SolidColorBrush(Colors.OrangeRed);

        ToggleBackendBtn.Content = usingD2D
            ? "Switch to DX9 (restart)"
            : "Switch to D2D (restart)";

        // Build all the tabs
        StartAnimations();
        BuildParticleSwarm((int)ParticleSlider.Value);
        BuildGradientGrid((int)GradientSlider.Value);
        BuildEffectsPanel((int)EffectsSlider.Value);
        BuildTextPanel();
        BuildImagePanel();
        BuildTransformCanvas((int)TransformSlider.Value);
        BuildD2DEffectsPanel();

        _isLoaded = true;
        UpdateShapeCount();
    }

    // ══════════════════════════════════════════════════════════════
    //  Toggle backend button
    // ══════════════════════════════════════════════════════════════
    private void ToggleBackend_Click(object sender, RoutedEventArgs e)
    {
        bool currentlyD2D = Environment.GetEnvironmentVariable("WPF_USE_D2D") == "1";
        string newVal = currentlyD2D ? "0" : "1";

        // Persist into the user-level env var so the next launch picks it up
        Environment.SetEnvironmentVariable("WPF_USE_D2D", newVal, EnvironmentVariableTarget.User);
        Environment.SetEnvironmentVariable("WPF_USE_D2D", newVal);

        // Re-launch ourselves
        var exe = Environment.ProcessPath!;
        Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
        Application.Current.Shutdown();
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 1 — Animated Shapes
    // ══════════════════════════════════════════════════════════════
    private void StartAnimations()
    {
        // Rotate gradient rect
        Animate(RectRotate, RotateTransform.AngleProperty, 0, 360, 6, forever: true);

        // Pulse the ellipse
        Animate(EllipseScale, ScaleTransform.ScaleXProperty, 0.7, 1.3, 1.5, autoReverse: true, forever: true);
        Animate(EllipseScale, ScaleTransform.ScaleYProperty, 1.3, 0.7, 1.5, autoReverse: true, forever: true);

        // Bounce ball
        var bounce = new DoubleAnimation(80, 450, TimeSpan.FromSeconds(1.2))
        {
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new BounceEase { Bounces = 3, Bounciness = 2 }
        };
        BounceBall.BeginAnimation(Canvas.TopProperty, bounce);

        // Fade bezier
        AnimateElement(BezierPath, OpacityProperty, 0.3, 1.0, 2, autoReverse: true, forever: true);

        // Spin stars at different speeds
        Animate(Star1Rotate, RotateTransform.AngleProperty, 0, 360, 3, forever: true);
        Animate(Star2Rotate, RotateTransform.AngleProperty, 360, 0, 5, forever: true);
        Animate(Star3Rotate, RotateTransform.AngleProperty, 0, 360, 4, forever: true);

        // Pulse rings
        PulseRing(Ring1, 0.3, 1.0, 2.0);
        PulseRing(Ring2, 0.2, 0.9, 2.5);
        PulseRing(Ring3, 0.1, 0.8, 3.0);
        PulseRing(Ring4, 0.05, 0.7, 3.5);

        // Scrolling ticker
        var tickerAnim = new DoubleAnimation(1200, -600, TimeSpan.FromSeconds(10))
        { RepeatBehavior = RepeatBehavior.Forever };
        TickerText.BeginAnimation(Canvas.LeftProperty, tickerAnim);

    }

    private void PulseRing(Ellipse ring, double from, double to, double sec)
    {
        AnimateElement(ring, OpacityProperty, from, to, sec, autoReverse: true, forever: true);
        var grow = new DoubleAnimation(ring.Width, ring.Width + 30, TimeSpan.FromSeconds(sec))
        { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever };
        ring.BeginAnimation(WidthProperty, grow);
        ring.BeginAnimation(HeightProperty, grow);
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 2 — Particle Swarm
    // ══════════════════════════════════════════════════════════════
    private void BuildParticleSwarm(int count)
    {
        Color[] palette = { Colors.Crimson, Colors.Gold, Colors.DeepSkyBlue,
                            Colors.MediumSpringGreen, Colors.Orchid, Colors.OrangeRed,
                            Colors.Turquoise, Colors.HotPink, Colors.Chartreuse };

        for (int i = 0; i < count; i++)
        {
            double size = 4 + _rng.NextDouble() * 14;
            var dot = new Ellipse
            {
                Width = size, Height = size,
                Fill = new SolidColorBrush(palette[i % palette.Length]) { Opacity = 0.5 + _rng.NextDouble() * 0.5 },
            };
            Canvas.SetLeft(dot, _rng.NextDouble() * 1100);
            Canvas.SetTop(dot, _rng.NextDouble() * 600);
            ParticleCanvas.Children.Add(dot);
            _particles.Add(dot);

            double speed = 0.5 + _rng.NextDouble() * 2.5;
            double angle = _rng.NextDouble() * Math.PI * 2;
            _velocities.Add(new Vector(Math.Cos(angle) * speed, Math.Sin(angle) * speed));
        }
        // Use CompositionTarget.Rendering for smooth per-frame updates
        CompositionTarget.Rendering += MoveParticles;
    }

    private void MoveParticles(object? sender, EventArgs e)
    {
        double w = ParticleCanvas.ActualWidth > 0 ? ParticleCanvas.ActualWidth : 1100;
        double h = ParticleCanvas.ActualHeight > 0 ? ParticleCanvas.ActualHeight : 600;

        for (int i = 0; i < _particles.Count; i++)
        {
            var p = _particles[i];
            var v = _velocities[i];
            double x = Canvas.GetLeft(p) + v.X;
            double y = Canvas.GetTop(p) + v.Y;

            if (x < 0 || x > w) { v.X = -v.X; x = Math.Clamp(x, 0, w); }
            if (y < 0 || y > h) { v.Y = -v.Y; y = Math.Clamp(y, 0, h); }

            _velocities[i] = v;
            Canvas.SetLeft(p, x);
            Canvas.SetTop(p, y);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 3 — Gradient Grid (many small gradient-filled rects)
    // ══════════════════════════════════════════════════════════════
    private void BuildGradientGrid(int count)
    {
        Color[] starts = { Colors.Crimson, Colors.DodgerBlue, Colors.Gold, Colors.MediumSpringGreen,
                           Colors.DarkOrchid, Colors.OrangeRed, Colors.DeepSkyBlue, Colors.HotPink };
        Color[] ends   = { Colors.MidnightBlue, Colors.DarkGreen, Colors.DarkRed, Colors.Teal,
                           Colors.MediumBlue, Colors.DarkSlateGray, Colors.Indigo, Colors.Maroon };

        for (int i = 0; i < count; i++)
        {
            double angle = (double)i / count * 360;
            var rect = new Border
            {
                Width = 70, Height = 70,
                Margin = new Thickness(3),
                CornerRadius = new CornerRadius(6),
                Background = new LinearGradientBrush(
                    starts[i % starts.Length],
                    ends[i % ends.Length],
                    angle),
                Child = new TextBlock
                {
                    Text = $"#{i}",
                    FontSize = 10,
                    Foreground = Brushes.White,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    Opacity = 0.7
                }
            };

            // Every 5th one gets a subtle animation
            if (i % 5 == 0)
            {
                var anim = new DoubleAnimation(0.5, 1.0, TimeSpan.FromSeconds(1 + (i % 3)))
                { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever };
                rect.BeginAnimation(OpacityProperty, anim);
            }

            GradientGrid.Children.Add(rect);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 4 — Effects Stress (drop shadows, blurs, etc.)
    // ══════════════════════════════════════════════════════════════
    private void BuildEffectsPanel(int count)
    {
        for (int i = 0; i < count; i++)
        {
            var border = new Border
            {
                Width = 90, Height = 90,
                Margin = new Thickness(6),
                CornerRadius = new CornerRadius(8),
                Background = new SolidColorBrush(Color.FromRgb(
                    (byte)(40 + i * 2 % 200),
                    (byte)(20 + i * 3 % 180),
                    (byte)(80 + i * 5 % 170))),
            };

            if (i % 3 == 0)
            {
                border.Effect = new DropShadowEffect
                {
                    Color = Colors.Crimson,
                    BlurRadius = 8 + i % 12,
                    ShadowDepth = 3,
                    Opacity = 0.7
                };
            }
            else if (i % 3 == 1)
            {
                border.Effect = new BlurEffect { Radius = 2 + i % 6 };
            }
            else
            {
                border.Effect = new DropShadowEffect
                {
                    Color = Colors.DeepSkyBlue,
                    BlurRadius = 15,
                    ShadowDepth = 0,
                    Opacity = 0.9
                };
            }

            border.Child = new TextBlock
            {
                Text = i % 3 == 0 ? "Shadow" : i % 3 == 1 ? "Blur" : "Glow",
                FontSize = 11,
                Foreground = Brushes.White,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };

            // Animate opacity on every other element
            if (i % 2 == 0)
            {
                var anim = new DoubleAnimation(0.4, 1.0, TimeSpan.FromSeconds(1.5 + i % 4))
                { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever };
                border.BeginAnimation(OpacityProperty, anim);
            }

            EffectsPanel.Children.Add(border);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 5 — Text Rendering
    // ══════════════════════════════════════════════════════════════
    private void BuildTextPanel()
    {
        string[] fonts = { "Segoe UI", "Consolas", "Arial", "Times New Roman",
                           "Trebuchet MS", "Georgia", "Verdana", "Courier New",
                           "Calibri", "Cambria", "Comic Sans MS", "Impact" };
        double[] sizes = { 10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 60 };
        string sampleText = "The quick brown fox jumps over the lazy dog. 0123456789 !@#$%";

        Color[] colors = { Colors.White, Colors.Crimson, Colors.Gold,
                           Colors.DeepSkyBlue, Colors.MediumSpringGreen,
                           Colors.Orchid, Colors.OrangeRed, Colors.Turquoise };

        int idx = 0;
        foreach (var font in fonts)
        {
            // Font header
            TextPanel.Children.Add(new TextBlock
            {
                Text = font,
                FontSize = 11,
                Foreground = new SolidColorBrush(Color.FromArgb(128, 255, 255, 255)),
                Margin = new Thickness(0, 12, 0, 2)
            });

            foreach (var size in sizes)
            {
                var tb = new TextBlock
                {
                    Text = sampleText,
                    FontFamily = new FontFamily(font),
                    FontSize = size,
                    Foreground = new SolidColorBrush(colors[idx % colors.Length]),
                    TextWrapping = TextWrapping.Wrap,
                    Margin = new Thickness(0, 1, 0, 1)
                };

                // Some with bold/italic
                if (idx % 4 == 1) tb.FontWeight = FontWeights.Bold;
                if (idx % 4 == 2) tb.FontStyle = FontStyles.Italic;
                if (idx % 4 == 3) { tb.FontWeight = FontWeights.Bold; tb.FontStyle = FontStyles.Italic; }

                TextPanel.Children.Add(tb);
                idx++;
            }
        }

        // Also add a block of sub-pixel positioned text
        var subpixelHeader = new TextBlock
        {
            Text = "Sub-pixel positioning test (look for jitter/aliasing):",
            FontSize = 13, Foreground = Brushes.Gray, Margin = new Thickness(0, 24, 0, 8)
        };
        TextPanel.Children.Add(subpixelHeader);

        for (int i = 0; i < 20; i++)
        {
            var tb = new TextBlock
            {
                Text = $"Line {i}: abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                FontSize = 13 + i * 0.1,
                FontFamily = new FontFamily("Segoe UI"),
                Foreground = Brushes.White,
                Margin = new Thickness(i * 0.5, 0, 0, 0)
            };
            TextPanel.Children.Add(tb);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 6 — Image Rendering
    // ══════════════════════════════════════════════════════════════
    private void BuildImagePanel()
    {
        // ── Section 1: Full-size images at native resolution ──────
        AddSectionHeader("Full-Size Images (native resolution)");

        // Panoramic banner (full width)
        AddImageWithLabel("Panoramic 1600×400", "Images/panoramic_1600x400.jpg",
            stretch: Stretch.Uniform, maxHeight: 300);

        // Large landscape
        AddImageWithLabel("Landscape 1920×1080", "Images/landscape_1920.jpg",
            stretch: Stretch.Uniform, maxHeight: 500);

        // ── Section 2: Various sizes & aspect ratios ─────────────
        AddSectionHeader("Various Sizes & Aspect Ratios");

        var sizePanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };

        // Square
        sizePanel.Children.Add(MakeImageCard("Square 800×800",
            "Images/square_800.jpg", 280, 280));

        // Portrait
        sizePanel.Children.Add(MakeImageCard("Portrait 600×900",
            "Images/portrait_600x900.jpg", 200, 300));

        // Medium
        sizePanel.Children.Add(MakeImageCard("Photo 1024×768",
            "Images/photo_1024x768.jpg", 320, 240));

        // Thumbnail
        sizePanel.Children.Add(MakeImageCard("Thumb 200×200",
            "Images/thumb_200.jpg", 150, 150));

        // PNG with alpha
        sizePanel.Children.Add(MakeImageCard("Alpha PNG",
            "Images/alpha_circles.png", 200, 200));

        ImagePanel.Children.Add(sizePanel);

        return;  // TEMP: isolate freeze

        // ── Section 3: Stretch modes ─────────────────────────────
        AddSectionHeader("Stretch Modes (same image, different modes)");

        var stretchPanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };
        foreach (var mode in new[] { Stretch.None, Stretch.Fill,
                                      Stretch.Uniform, Stretch.UniformToFill })
        {
            var border = new Border
            {
                Width = 260, Height = 200,
                Margin = new Thickness(6),
                BorderBrush = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4)),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(6),
                ClipToBounds = true,
                Background = new SolidColorBrush(Color.FromRgb(0x0d, 0x0d, 0x1a))
            };
            var stack = new StackPanel();
            stack.Children.Add(new TextBlock
            {
                Text = $"Stretch.{mode}",
                FontSize = 11, Foreground = Brushes.White,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 4, 0, 4)
            });
            var img = new System.Windows.Controls.Image
            {
                Source = GetCachedBitmap("Images/landscape_1920.jpg", 520),
                Stretch = mode,
                Height = 160
            };
            stack.Children.Add(img);
            border.Child = stack;
            stretchPanel.Children.Add(border);
        }
        ImagePanel.Children.Add(stretchPanel);

        // ── Section 4: Image with effects ────────────────────────
        AddSectionHeader("Images with Effects");

        var effectsPanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };

        // Drop shadow on image
        effectsPanel.Children.Add(MakeEffectImageCard("Drop Shadow",
            "Images/square_800.jpg", 220, 220,
            new DropShadowEffect { Color = Colors.Crimson, BlurRadius = 20, ShadowDepth = 8, Opacity = 0.8 }));

        // Blur on image
        effectsPanel.Children.Add(MakeEffectImageCard("Blur (r=5)",
            "Images/photo_1024x768.jpg", 260, 200,
            new BlurEffect { Radius = 5 }));

        // Glow (zero-depth shadow)
        effectsPanel.Children.Add(MakeEffectImageCard("Glow",
            "Images/portrait_600x900.jpg", 180, 270,
            new DropShadowEffect { Color = Colors.DeepSkyBlue, BlurRadius = 25, ShadowDepth = 0, Opacity = 0.9 }));

        // Alpha PNG with drop shadow
        effectsPanel.Children.Add(MakeEffectImageCard("Alpha + Shadow",
            "Images/alpha_circles.png", 200, 200,
            new DropShadowEffect { Color = Colors.Gold, BlurRadius = 15, ShadowDepth = 5 }));

        ImagePanel.Children.Add(effectsPanel);

        // ── Section 5: Animated image transforms ─────────────────
        AddSectionHeader("Animated Image Transforms");

        var animPanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };

        // Spinning image
        {
            var img = MakeImage("Images/thumb_200.jpg", 150, 150);
            var rot = new RotateTransform(0, 75, 75);
            img.RenderTransform = rot;
            Animate(rot, RotateTransform.AngleProperty, 0, 360, 6, forever: true);
            var card = WrapInCard("Rotating", img, 170, 190);
            animPanel.Children.Add(card);
        }

        // Pulsing scale image
        {
            var img = MakeImage("Images/square_800.jpg", 140, 140);
            var scl = new ScaleTransform(1, 1, 70, 70);
            img.RenderTransform = scl;
            Animate(scl, ScaleTransform.ScaleXProperty, 0.7, 1.1, 2, autoReverse: true, forever: true);
            Animate(scl, ScaleTransform.ScaleYProperty, 0.7, 1.1, 2, autoReverse: true, forever: true);
            var card = WrapInCard("Pulsing", img, 170, 190);
            animPanel.Children.Add(card);
        }

        // Fading opacity image
        {
            var img = MakeImage("Images/photo_1024x768.jpg", 200, 150);
            AnimateElement(img, OpacityProperty, 0.2, 1.0, 2.5, autoReverse: true, forever: true);
            var card = WrapInCard("Fading", img, 220, 190);
            animPanel.Children.Add(card);
        }

        // Skew transform image
        {
            var img = MakeImage("Images/panoramic_1600x400.jpg", 240, 80);
            var skew = new SkewTransform(0, 0, 120, 40);
            img.RenderTransform = skew;
            Animate(skew, SkewTransform.AngleXProperty, -15, 15, 3, autoReverse: true, forever: true);
            var card = WrapInCard("Skewing", img, 280, 140);
            animPanel.Children.Add(card);
        }

        ImagePanel.Children.Add(animPanel);

        // ── Section 6: Image brush fills ─────────────────────────
        AddSectionHeader("Image Brush Fills (TileBrush modes)");

        var brushPanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };

        foreach (var tile in new[] { TileMode.None, TileMode.Tile,
                                      TileMode.FlipX, TileMode.FlipY, TileMode.FlipXY })
        {
            var imgBrush = new ImageBrush
            {
                ImageSource = GetCachedBitmap("Images/thumb_200.jpg"),
                TileMode = tile,
                Viewport = new Rect(0, 0, 0.3, 0.3),
                ViewportUnits = BrushMappingMode.RelativeToBoundingBox
            };
            var rect = new Border
            {
                Width = 200, Height = 200,
                Margin = new Thickness(6),
                CornerRadius = new CornerRadius(8),
                Background = imgBrush,
                BorderBrush = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4)),
                BorderThickness = new Thickness(1)
            };
            var stack = new StackPanel();
            stack.Children.Add(new TextBlock
            {
                Text = $"TileMode.{tile}",
                FontSize = 11, Foreground = Brushes.White,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 2, 0, 0)
            });
            stack.Children.Add(rect);
            brushPanel.Children.Add(new Border
            {
                Margin = new Thickness(4),
                Child = stack
            });
        }
        ImagePanel.Children.Add(brushPanel);

        // ── Section 7: Opacity mask with image ───────────────────
        AddSectionHeader("Opacity Mask (image as mask)");

        var maskPanel = new WrapPanel { Margin = new Thickness(0, 4, 0, 16) };
        {
            // Use the alpha PNG as an opacity mask over a gradient
            var border = new Border
            {
                Width = 300, Height = 300,
                Margin = new Thickness(6),
                CornerRadius = new CornerRadius(8),
                Background = new LinearGradientBrush(
                    Color.FromRgb(0xe9, 0x45, 0x60),
                    Color.FromRgb(0x0f, 0x34, 0x60), 45),
                OpacityMask = new ImageBrush
                {
                    ImageSource = GetCachedBitmap("Images/alpha_circles.png")
                }
            };
            border.Child = new TextBlock
            {
                Text = "Gradient masked\nby alpha PNG",
                FontSize = 16, FontWeight = FontWeights.Bold,
                Foreground = Brushes.White,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                TextAlignment = TextAlignment.Center
            };
            maskPanel.Children.Add(border);

            // Image clipped by ellipse geometry
            var clipped = MakeImage("Images/landscape_1920.jpg", 300, 300);
            clipped.Clip = new EllipseGeometry(new Point(150, 150), 140, 140);
            maskPanel.Children.Add(WrapInCard("Ellipse Clip", clipped, 320, 340));
        }
        ImagePanel.Children.Add(maskPanel);
    }

    // Image tab helper methods
    private void AddSectionHeader(string text)
    {
        ImagePanel.Children.Add(new TextBlock
        {
            Text = text,
            FontSize = 18, FontWeight = FontWeights.Bold,
            Foreground = new SolidColorBrush(Color.FromRgb(0xe9, 0x45, 0x60)),
            Margin = new Thickness(0, 20, 0, 8)
        });
    }

    private void AddImageWithLabel(string label, string resourcePath,
        Stretch stretch = Stretch.Uniform, double maxHeight = double.PositiveInfinity)
    {
        var stack = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
        stack.Children.Add(new TextBlock
        {
            Text = label, FontSize = 12, Foreground = Brushes.Gray,
            Margin = new Thickness(0, 0, 0, 4)
        });
        var img = new System.Windows.Controls.Image
        {
            Source = GetCachedBitmap(resourcePath),
            Stretch = stretch,
            MaxHeight = maxHeight,
            HorizontalAlignment = HorizontalAlignment.Left
        };
        var border = new Border
        {
            BorderBrush = new SolidColorBrush(Color.FromArgb(60, 255, 255, 255)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(4),
            ClipToBounds = true,
            Child = img
        };
        stack.Children.Add(border);
        ImagePanel.Children.Add(stack);
    }

    private Border MakeImageCard(string label, string resourcePath, double w, double h)
    {
        var stack = new StackPanel();
        stack.Children.Add(new TextBlock
        {
            Text = label, FontSize = 11, Foreground = Brushes.White,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 4, 0, 4)
        });
        stack.Children.Add(MakeImage(resourcePath, w - 20, h - 30));
        return new Border
        {
            Width = w, Height = h,
            Margin = new Thickness(6),
            CornerRadius = new CornerRadius(6),
            Background = new SolidColorBrush(Color.FromRgb(0x0d, 0x0d, 0x1a)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4)),
            BorderThickness = new Thickness(1),
            ClipToBounds = true,
            Child = stack
        };
    }

    private Border MakeEffectImageCard(string label, string resourcePath,
        double w, double h, Effect effect)
    {
        var img = MakeImage(resourcePath, w - 30, h - 40);
        img.Effect = effect;
        return WrapInCard(label, img, w, h);
    }

    private BitmapImage GetCachedBitmap(string resourcePath, int decodeWidth = 0)
    {
        string key = $"{resourcePath}@{decodeWidth}";
        if (_imageCache.TryGetValue(key, out var cached))
            return cached;

        var bmp = new BitmapImage();
        bmp.BeginInit();
        bmp.UriSource = new Uri($"pack://application:,,,/{resourcePath}");
        bmp.CacheOption = BitmapCacheOption.OnLoad;
        if (decodeWidth > 0)
            bmp.DecodePixelWidth = decodeWidth;
        bmp.EndInit();
        bmp.Freeze();
        _imageCache[key] = bmp;
        return bmp;
    }

    private System.Windows.Controls.Image MakeImage(string resourcePath, double w, double h)
    {
        // Decode at ~2x display width for quality on HiDPI, capped at reasonable sizes
        int decodeW = Math.Min((int)(w * 2), 1024);
        return new System.Windows.Controls.Image
        {
            Source = GetCachedBitmap(resourcePath, decodeW),
            Width = w, Height = h,
            Stretch = Stretch.Uniform
        };
    }

    private Border WrapInCard(string label, UIElement content, double w, double h)
    {
        var stack = new StackPanel();
        stack.Children.Add(new TextBlock
        {
            Text = label, FontSize = 11, Foreground = Brushes.White,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 4, 0, 4)
        });
        stack.Children.Add(content);
        return new Border
        {
            Width = w, Height = h,
            Margin = new Thickness(6),
            CornerRadius = new CornerRadius(6),
            Background = new SolidColorBrush(Color.FromRgb(0x0d, 0x0d, 0x1a)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4)),
            BorderThickness = new Thickness(1),
            ClipToBounds = true,
            Child = stack
        };
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 7 — Opacity + Transforms
    // ══════════════════════════════════════════════════════════════
    private void BuildTransformCanvas(int count)
    {
        Color[] palette = { Colors.Crimson, Colors.DodgerBlue, Colors.Gold,
                            Colors.MediumSpringGreen, Colors.DarkOrchid,
                            Colors.OrangeRed, Colors.DeepSkyBlue, Colors.HotPink };

        for (int i = 0; i < count; i++)
        {
            double cx = 60 + _rng.NextDouble() * 1020;
            double cy = 30 + _rng.NextDouble() * 550;
            double size = 20 + _rng.NextDouble() * 60;

            FrameworkElement shape;
            int kind = i % 4;
            if (kind == 0)
            {
                shape = new Rectangle
                {
                    Width = size, Height = size,
                    Fill = new SolidColorBrush(palette[i % palette.Length]) { Opacity = 0.4 + _rng.NextDouble() * 0.5 },
                    RadiusX = 4, RadiusY = 4,
                    Stroke = new SolidColorBrush(Colors.White) { Opacity = 0.3 },
                    StrokeThickness = 1
                };
            }
            else if (kind == 1)
            {
                shape = new Ellipse
                {
                    Width = size, Height = size * 0.7,
                    Fill = new SolidColorBrush(palette[i % palette.Length]) { Opacity = 0.5 + _rng.NextDouble() * 0.4 },
                };
            }
            else if (kind == 2)
            {
                shape = new Path
                {
                    Data = Geometry.Parse($"M 0,{size} L {size / 2},0 L {size},0 L {size},0 L {size},{size} Z"),
                    Fill = new SolidColorBrush(palette[i % palette.Length]) { Opacity = 0.5 },
                    Stroke = Brushes.White, StrokeThickness = 0.5
                };
            }
            else
            {
                // Text shape
                shape = new TextBlock
                {
                    Text = "WPF",
                    FontSize = size * 0.5,
                    FontWeight = FontWeights.Bold,
                    Foreground = new SolidColorBrush(palette[i % palette.Length]),
                };
            }

            Canvas.SetLeft(shape, cx);
            Canvas.SetTop(shape, cy);

            // Compose transforms: rotate + scale
            var tg = new TransformGroup();
            var rot = new RotateTransform(0, size / 2, size / 2);
            var scl = new ScaleTransform(1, 1, size / 2, size / 2);
            tg.Children.Add(rot);
            tg.Children.Add(scl);
            shape.RenderTransform = tg;

            // Spin animation
            double dur = 3 + _rng.NextDouble() * 8;
            double fromAngle = i % 2 == 0 ? 0 : 360;
            double toAngle = i % 2 == 0 ? 360 : 0;
            Animate(rot, RotateTransform.AngleProperty, fromAngle, toAngle, dur, forever: true);

            // Breathe scale
            double sFrom = 0.6 + _rng.NextDouble() * 0.3;
            double sTo = 1.0 + _rng.NextDouble() * 0.4;
            Animate(scl, ScaleTransform.ScaleXProperty, sFrom, sTo, dur * 0.6, autoReverse: true, forever: true);
            Animate(scl, ScaleTransform.ScaleYProperty, sFrom, sTo, dur * 0.6, autoReverse: true, forever: true);

            // Opacity pulse
            AnimateElement(shape, OpacityProperty, 0.3 + _rng.NextDouble() * 0.3, 0.8 + _rng.NextDouble() * 0.2,
                    dur * 0.4, autoReverse: true, forever: true);

            TransformCanvas.Children.Add(shape);
            _spinners.Add(shape);
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  TAB 8 — D2D Built-in Effects — Interactive Showcase
    //  (loaded via reflection since the D2D types only exist
    //  in our locally-built PresentationCore)
    // ══════════════════════════════════════════════════════════════
    private void BuildD2DEffectsPanel()
    {
        var asm = typeof(Effect).Assembly;

        var tD2DEffect   = asm.GetType("System.Windows.Media.Effects.D2DEffect");
        var tD2DEffects  = asm.GetType("System.Windows.Media.Effects.D2DEffects");
        var tBlur        = asm.GetType("System.Windows.Media.Effects.D2DGaussianBlurEffect");
        var tSat         = asm.GetType("System.Windows.Media.Effects.D2DSaturationEffect");
        var tHue         = asm.GetType("System.Windows.Media.Effects.D2DHueRotationEffect");
        var tShadow      = asm.GetType("System.Windows.Media.Effects.D2DShadowEffect");

        if (tD2DEffect == null)
        {
            D2DEffectsPanel.Children.Add(new TextBlock
            {
                Text = "D2D effect types not found in this PresentationCore build.\n" +
                       "Deploy the locally-built PresentationCore.dll to the runtime folder.",
                FontSize = 14, Foreground = Brushes.OrangeRed,
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 20, 0, 0)
            });
            return;
        }

        // ── Theme colors ─────────────────────────────────────────
        var accentBrush = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4));
        var cardBg      = new SolidColorBrush(Color.FromRgb(0x16, 0x21, 0x3e));
        var sectionBg   = new SolidColorBrush(Color.FromRgb(0x0f, 0x17, 0x2a));
        var ctrlPanelBg = new SolidColorBrush(Color.FromRgb(0x1a, 0x28, 0x48));
        var borderClr   = new SolidColorBrush(Color.FromRgb(0x2a, 0x3a, 0x5c));

        // ── Reflection helpers ───────────────────────────────────
        Effect? CreateTyped(Type? t) =>
            t != null ? (Effect?)Activator.CreateInstance(t) : null;

        void SetProp(object obj, string name, object value) =>
            obj.GetType().GetProperty(name)?.SetValue(obj, value);

        Guid GetClsid(string field) =>
            (Guid)(tD2DEffects!.GetField(field,
                BindingFlags.Public | BindingFlags.Static)
                ?.GetValue(null) ?? Guid.Empty);

        Effect? CreateGeneric(Guid clsid)
        {
            var ctor = tD2DEffect!.GetConstructor(new[] { typeof(Guid) });
            return (Effect?)ctor?.Invoke(new object[] { clsid });
        }

        void CallSetValue(object fx, int idx, float val)
        {
            var m = fx.GetType().GetMethod("SetValue",
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                null, new[] { typeof(int), typeof(float) }, null);
            m?.Invoke(fx, new object[] { idx, val });
        }

        void CallSetValueInt(object fx, int idx, int val)
        {
            var m = fx.GetType().GetMethod("SetValue",
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                null, new[] { typeof(int), typeof(int) }, null);
            m?.Invoke(fx, new object[] { idx, val });
        }

        void CallSetInputEffect(object fx, int idx, Effect inputFx)
        {
            var m = tD2DEffect!.GetMethod("SetInput",
                BindingFlags.Instance | BindingFlags.Public,
                null, new[] { typeof(int), tD2DEffect }, null);
            m?.Invoke(fx, new object[] { idx, inputFx });
        }

        void CallSetInputBrush(object fx, int idx, Brush brush)
        {
            var m = tD2DEffect!.GetMethod("SetInput",
                BindingFlags.Instance | BindingFlags.Public,
                null, new[] { typeof(int), typeof(Brush) }, null);
            m?.Invoke(fx, new object[] { idx, brush });
        }

        // ── Target element factories ─────────────────────────────
        var gradient = new LinearGradientBrush(
            Color.FromRgb(0xe9, 0x45, 0x60),
            Color.FromRgb(0x0f, 0x34, 0x60), 45);

        System.Windows.Controls.Image MakeImg(string path, double w, double h)
        {
            int dec = Math.Min((int)(w * 2), 1024);
            return new System.Windows.Controls.Image
            {
                Source = GetCachedBitmap(path, dec),
                Width = w, Height = h,
                Stretch = Stretch.UniformToFill,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };
        }

        Rectangle MakeGradientRect(double w, double h) => new Rectangle
        {
            Width = w, Height = h, RadiusX = 6, RadiusY = 6,
            Fill = gradient.Clone()
        };

        UIElement MakeRichText() => new TextBlock
        {
            Text = "WPF + D2D", FontSize = 26,
            FontWeight = FontWeights.Bold,
            Foreground = new LinearGradientBrush(Colors.Gold, Colors.OrangeRed, 0),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center
        };

        UIElement MakeUIControls()
        {
            var sp = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center };
            sp.Children.Add(new Button
            {
                Content = "Button", FontSize = 12,
                Padding = new Thickness(14, 5, 14, 5),
                Margin = new Thickness(0, 2, 0, 2),
                Background = new LinearGradientBrush(Colors.DodgerBlue, Colors.MediumPurple, 45),
                Foreground = Brushes.White, BorderBrush = Brushes.Transparent
            });
            sp.Children.Add(new ProgressBar
            {
                Value = 65, Height = 14, Width = 110,
                Margin = new Thickness(0, 2, 0, 2),
                Foreground = new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4))
            });
            sp.Children.Add(new TextBlock
            {
                Text = "Label text", Foreground = Brushes.LightGoldenrodYellow,
                FontSize = 12, Margin = new Thickness(0, 2, 0, 0)
            });
            return sp;
        }

        var galleryTargets = new (string Label, Func<UIElement> Make,
            double CW, double CH)[]
        {
            ("Landscape",   () => MakeImg("Images/landscape_1920.jpg", 140, 90), 160, 130),
            ("Portrait",    () => MakeImg("Images/portrait_600x900.jpg", 70, 105), 90, 145),
            ("Square",      () => MakeImg("Images/square_800.jpg", 100, 100), 120, 140),
            ("Photo",       () => MakeImg("Images/photo_1024x768.jpg", 140, 105), 160, 145),
            ("Alpha PNG",   () => MakeImg("Images/alpha_circles.png", 100, 100), 120, 140),
            ("Gradient",    () => (UIElement)MakeGradientRect(130, 85), 150, 125),
            ("Rich Text",   () => MakeRichText(), 160, 70),
            ("UI Controls", () => MakeUIControls(), 150, 120),
        };

        Border MakeCard(string label, UIElement child, double w, double h)
        {
            var dock = new DockPanel();
            var lbl = new TextBlock
            {
                Text = label, FontSize = 10, Foreground = Brushes.LightGray,
                HorizontalAlignment = HorizontalAlignment.Center,
                Margin = new Thickness(0, 4, 0, 2)
            };
            DockPanel.SetDock(lbl, Dock.Top);
            dock.Children.Add(lbl);
            dock.Children.Add(new Border
            {
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Child = child
            });
            return new Border
            {
                Width = w, Height = h, Margin = new Thickness(4),
                CornerRadius = new CornerRadius(6),
                Background = cardBg, BorderBrush = borderClr,
                BorderThickness = new Thickness(1),
                ClipToBounds = true, Child = dock
            };
        }

        // ── Image catalog ────────────────────────────────────────
        var imageCatalog = new (string Label, string Path)[]
        {
            ("Landscape (1920\u00d71080)",  "Images/landscape_1920.jpg"),
            ("Square (800\u00d7800)",       "Images/square_800.jpg"),
            ("Portrait (600\u00d7900)",     "Images/portrait_600x900.jpg"),
            ("Photo (1024\u00d7768)",       "Images/photo_1024x768.jpg"),
            ("Panoramic (1600\u00d7400)",   "Images/panoramic_1600x400.jpg"),
            ("Alpha Circles (PNG)",          "Images/alpha_circles.png"),
        };
        int selImgIdx = 0;

        // ── Slider-definition helpers ────────────────────────────
        (string, double, double, double, string, Action<Effect, double>)
        PropSlider(string label, string prop, double min, double max,
            double def, string fmt = "F2")
            => (label, min, max, def, fmt,
                (fx, v) => SetProp(fx, prop, (float)v));

        (string, double, double, double, string, Action<Effect, double>)
        PropIntSlider(string label, string prop, double min, double max,
            double def)
            => (label, min, max, def, "F0",
                (fx, v) => SetProp(fx, prop, (int)Math.Round(v)));

        (string, double, double, double, string, Action<Effect, double>)
        ValSlider(string label, int index, double min, double max,
            double def, string fmt = "F2")
            => (label, min, max, def, fmt,
                (fx, v) => CallSetValue(fx, index, (float)v));

        (string, double, double, double, string, Action<Effect, double>)
        IntValSlider(string label, int index, double min, double max,
            double def)
            => (label, min, max, def, "F0",
                (fx, v) => CallSetValueInt(fx, index, (int)Math.Round(v)));

        var emptySliders = Array.Empty<(string, double, double, double, string,
            Action<Effect, double>)>();

        // Shadow color state (persists across rebuilds)
        Color shadowColor = Colors.Black;

        // Effect chain state (captured references to inner effects)
        Effect? chainBlurRef = null;
        Effect? chainSepiaRef = null;

        // Blend mode state
        int blendModeIdx = 0;
        float blendHueAngle = 120f;
        Effect? blendFgRef = null;
        var blendModeNames = new[]
        {
            "Multiply", "Screen", "Darken", "Lighten",
            "Color Burn", "Color Dodge", "Linear Burn", "Linear Dodge",
            "Darker Color", "Lighter Color",
            "Overlay", "Soft Light", "Hard Light", "Vivid Light",
            "Linear Light", "Pin Light", "Hard Mix",
            "Difference", "Exclusion",
            "Hue", "Saturation", "Color", "Luminosity",
            "Dissolve", "Subtract", "Division"
        };

        // ═══════════════════════════════════════════════════════════
        //  Effect definitions — one entry per effect
        // ═══════════════════════════════════════════════════════════
        var effectDefs = new (
            string Name,
            Func<Effect?> Factory,
            (string Label, double Min, double Max, double Def, string Fmt,
             Action<Effect, double> Apply)[] Sliders,
            Action<StackPanel, Action>? ExtraControls,
            Action<Effect>? InitExtra)[]
        {
            // ── Typed effects ────────────────────────────────────
            ("Gaussian Blur",
                () => CreateTyped(tBlur),
                new[] {
                    PropSlider("Standard Deviation (\u03c3)", "StandardDeviation", 0, 50, 3, "F1"),
                    PropIntSlider("Border Mode (0=Soft, 1=Hard)", "BorderMode", 0, 1, 0),
                    PropIntSlider("Optimization (0=Speed, 1=Balanced, 2=Quality)", "Optimization", 0, 2, 1),
                }, null, null),

            ("Saturation",
                () => CreateTyped(tSat),
                new[] { PropSlider("Saturation", "Saturation", 0, 1, 0.5) },
                null, null),

            ("Hue Rotation",
                () => CreateTyped(tHue),
                new[] { PropSlider("Angle (\u00b0)", "Angle", 0, 360, 0, "F0") },
                null, null),

            ("Shadow",
                () => CreateTyped(tShadow),
                new[] {
                    PropSlider("Blur Std Deviation", "BlurStandardDeviation", 0, 20, 3, "F1"),
                    PropIntSlider("Optimization (0=Speed, 1=Balanced, 2=Quality)", "Optimization", 0, 2, 1),
                },
                (StackPanel panel, Action rebuild) =>
                {
                    panel.Children.Add(new TextBlock
                    {
                        Text = "Shadow Color", FontSize = 12,
                        Foreground = Brushes.LightGray,
                        Margin = new Thickness(0, 8, 0, 4)
                    });
                    var colorRow = new WrapPanel();
                    foreach (var (name, c) in new (string, Color)[]
                    {
                        ("Black",   Colors.Black),
                        ("Crimson", Colors.Crimson),
                        ("Blue",    Colors.DeepSkyBlue),
                        ("Gold",    Colors.Gold),
                        ("Green",   Colors.LimeGreen),
                        ("Purple",  Colors.MediumPurple),
                    })
                    {
                        var col = c;
                        var btn = new Button
                        {
                            Content = name, FontSize = 11,
                            Padding = new Thickness(8, 3, 8, 3),
                            Margin = new Thickness(0, 0, 4, 4),
                            Background = new SolidColorBrush(c),
                            Foreground = c == Colors.Black ? Brushes.White : Brushes.Black,
                            BorderBrush = Brushes.Transparent
                        };
                        btn.Click += (_, _) => { shadowColor = col; rebuild(); };
                        colorRow.Children.Add(btn);
                    }
                    panel.Children.Add(colorRow);
                },
                fx => SetProp(fx, "ShadowColor", shadowColor)),

            // ── Generic effects (by CLSID) ───────────────────────
            ("Sepia",
                () => CreateGeneric(GetClsid("Sepia")),
                new[] { ValSlider("Intensity", 0, 0, 1, 0.5) },
                null, null),

            ("Grayscale",
                () => CreateGeneric(GetClsid("Grayscale")),
                emptySliders, null, null),

            ("Invert",
                () => CreateGeneric(GetClsid("Invert")),
                emptySliders, null, null),

            ("Contrast",
                () => CreateGeneric(GetClsid("Contrast")),
                new[] { ValSlider("Contrast", 0, -1, 1, 0) },
                null, null),

            ("Exposure",
                () => CreateGeneric(GetClsid("Exposure")),
                new[] { ValSlider("Exposure Value", 0, -2, 2, 0) },
                null, null),

            ("Sharpen",
                () => CreateGeneric(GetClsid("Sharpen")),
                new[] {
                    ValSlider("Sharpness", 0, 0, 10, 0, "F1"),
                    ValSlider("Threshold", 1, 0, 1, 0.5),
                }, null, null),

            ("Temperature & Tint",
                () => CreateGeneric(GetClsid("TemperatureAndTint")),
                new[] {
                    ValSlider("Temperature", 0, -1, 1, 0),
                    ValSlider("Tint", 1, -1, 1, 0),
                }, null, null),

            ("Straighten",
                () => CreateGeneric(GetClsid("Straighten")),
                new[] { ValSlider("Angle", 0, -45, 45, 0, "F1") },
                null, null),

            ("Highlights & Shadows",
                () => CreateGeneric(GetClsid("HighlightsAndShadows")),
                new[] {
                    ValSlider("Highlights", 0, -1, 1, 0),
                    ValSlider("Shadows", 1, -1, 1, 0),
                    ValSlider("Clarity", 2, -1, 1, 0),
                }, null, null),

            ("Posterize",
                () => CreateGeneric(GetClsid("Posterize")),
                new[] {
                    IntValSlider("Red Values", 0, 2, 16, 4),
                    IntValSlider("Green Values", 1, 2, 16, 4),
                    IntValSlider("Blue Values", 2, 2, 16, 4),
                }, null, null),

            ("Vignette",
                () => CreateGeneric(GetClsid("Vignette")),
                new[] {
                    ValSlider("Transition Size", 1, 0, 1, 0.1),
                    ValSlider("Strength", 2, 0, 1, 0.5),
                }, null, null),

            // ── Effect chaining ──────────────────────────────────
            ("Effect Chain (Blur \u2192 Sepia)",
                () =>
                {
                    var blur = CreateGeneric(GetClsid("GaussianBlur"));
                    chainBlurRef = blur;
                    // blur input 0 = ImplicitInput (default)

                    var sepia = CreateGeneric(GetClsid("Sepia"));
                    chainSepiaRef = sepia;
                    // sepia input 0 = blur (chaining!)
                    if (blur != null && sepia != null)
                        CallSetInputEffect(sepia, 0, blur);

                    return sepia;
                },
                new[] {
                    ("Blur \u03c3", 0d, 50d, 3d, "F1",
                        (Action<Effect, double>)((_, v) =>
                        { if (chainBlurRef != null) CallSetValue(chainBlurRef, 0, (float)v); })),
                    ("Sepia Intensity", 0d, 1d, 0.5d, "F2",
                        (Action<Effect, double>)((_, v) =>
                        { if (chainSepiaRef != null) CallSetValue(chainSepiaRef, 0, (float)v); })),
                },
                null, null),

            // ── Blend modes ──────────────────────────────────────
            ("Blend Modes",
                () =>
                {
                    // Foreground = hue-rotated version of source
                    var hue = CreateGeneric(GetClsid("HueRotation"));
                    blendFgRef = hue;
                    if (hue != null)
                        CallSetValue(hue, 0, blendHueAngle);

                    // Blend: input 0 = original (implicit), input 1 = hue-rotated
                    var blend = CreateGeneric(GetClsid("Blend"));
                    if (blend != null)
                    {
                        CallSetValueInt(blend, 0, blendModeIdx);
                        CallSetInputBrush(blend, 0, Effect.ImplicitInput);
                        if (hue != null)
                            CallSetInputEffect(blend, 1, hue);
                    }
                    return blend;
                },
                new[] {
                    ("Hue Rotation Angle (\u00b0)", 0d, 360d, 120d, "F0",
                        (Action<Effect, double>)((_, v) =>
                        {
                            blendHueAngle = (float)v;
                        })),
                },
                (StackPanel panel, Action rebuild) =>
                {
                    panel.Children.Add(new TextBlock
                    {
                        Text = "Blend Mode", FontSize = 12,
                        Foreground = Brushes.LightGray,
                        Margin = new Thickness(0, 8, 0, 4)
                    });
                    var modeCb = new ComboBox { Width = 200, FontSize = 12 };
                    foreach (var name in blendModeNames) modeCb.Items.Add(name);
                    modeCb.SelectedIndex = blendModeIdx;
                    modeCb.SelectionChanged += (_, _) =>
                    {
                        blendModeIdx = modeCb.SelectedIndex;
                        rebuild();
                    };
                    panel.Children.Add(modeCb);
                },
                null),
        };

        // ═══════════════════════════════════════════════════════════
        //  UI Layout
        // ═══════════════════════════════════════════════════════════

        D2DEffectsPanel.Children.Add(new TextBlock
        {
            Text = "D2D Effects Showcase", FontSize = 24,
            FontWeight = FontWeights.Bold, Foreground = accentBrush,
            Margin = new Thickness(0, 4, 0, 12)
        });

        // Selector row: effect dropdown + image dropdown
        var selectorRow = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Margin = new Thickness(0, 0, 0, 16)
        };
        selectorRow.Children.Add(new TextBlock
        {
            Text = "Effect: ", FontSize = 14, Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 6, 0)
        });
        var effectCombo = new ComboBox { Width = 220, FontSize = 13 };
        foreach (var def in effectDefs) effectCombo.Items.Add(def.Name);
        effectCombo.SelectedIndex = 0;
        selectorRow.Children.Add(effectCombo);

        selectorRow.Children.Add(new TextBlock
        {
            Text = "Preview Image: ", FontSize = 14, Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(24, 0, 6, 0)
        });
        var imgCombo = new ComboBox { Width = 220, FontSize = 13 };
        foreach (var (lbl, _) in imageCatalog) imgCombo.Items.Add(lbl);
        imgCombo.SelectedIndex = 0;
        selectorRow.Children.Add(imgCombo);

        D2DEffectsPanel.Children.Add(selectorRow);

        // Content host (rebuilt when effect or image changes)
        var contentHost = new StackPanel();
        D2DEffectsPanel.Children.Add(contentHost);

        // ═══════════════════════════════════════════════════════════
        //  Saved slider values per effect index (survives rebuilds)
        // ═══════════════════════════════════════════════════════════
        var savedValues = new Dictionary<int, double[]>();

        void RebuildContent()
        {
            contentHost.Children.Clear();

            int ei = effectCombo.SelectedIndex;
            if (ei < 0 || ei >= effectDefs.Length) return;

            var def = effectDefs[ei];

            // Restore saved slider values or use defaults
            if (!savedValues.TryGetValue(ei, out var curVals))
            {
                curVals = def.Sliders.Select(s => s.Def).ToArray();
                savedValues[ei] = curVals;
            }

            var allPairs = new List<(UIElement El, Effect? Fx)>();

            Effect? FreshFx()
            {
                var fx = def.Factory();
                if (fx == null) return null;
                for (int k = 0; k < def.Sliders.Length; k++)
                    def.Sliders[k].Apply(fx, curVals[k]);
                def.InitExtra?.Invoke(fx);
                return fx;
            }

            void RebuildEffects()
            {
                for (int k = 0; k < allPairs.Count; k++)
                {
                    var (el, _) = allPairs[k];
                    var newFx = FreshFx();
                    el.Effect = newFx;
                    allPairs[k] = (el, newFx);
                }
            }

            Effect? MakeFx(UIElement target)
            {
                var fx = FreshFx();
                allPairs.Add((target, fx));
                return fx;
            }

            // Section wrapper
            var sectionBorder = new Border
            {
                Background = sectionBg, CornerRadius = new CornerRadius(8),
                Padding = new Thickness(16), Margin = new Thickness(0, 0, 0, 14),
                BorderBrush = borderClr, BorderThickness = new Thickness(1)
            };
            var sectionStack = new StackPanel();

            sectionStack.Children.Add(new TextBlock
            {
                Text = def.Name, FontSize = 20,
                FontWeight = FontWeights.Bold, Foreground = accentBrush,
                Margin = new Thickness(0, 0, 0, 12)
            });

            // ── Controls + Preview row ───────────────────────────
            var row = new Grid { Margin = new Thickness(0, 0, 0, 12) };
            bool hasControls = def.Sliders.Length > 0 || def.ExtraControls != null;

            if (hasControls)
            {
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

                var ctrlStack = new StackPanel();
                ctrlStack.Children.Add(new TextBlock
                {
                    Text = "Parameters", FontSize = 14,
                    FontWeight = FontWeights.SemiBold,
                    Foreground = Brushes.White,
                    Margin = new Thickness(0, 0, 0, 8)
                });

                for (int si = 0; si < def.Sliders.Length; si++)
                {
                    int capIdx = si;
                    var cap = def.Sliders[si];
                    bool isInt = cap.Fmt == "F0";
                    var valText = new TextBlock
                    {
                        Text = $"{cap.Label}: {curVals[si].ToString(cap.Fmt)}",
                        FontSize = 12, Foreground = Brushes.LightGray,
                        Margin = new Thickness(0, 4, 0, 2)
                    };
                    var slider = new Slider
                    {
                        Minimum = cap.Min, Maximum = cap.Max,
                        Value = curVals[si],
                        IsSnapToTickEnabled = isInt,
                        SmallChange = isInt ? 1 : (cap.Max - cap.Min) / 200,
                        LargeChange = isInt ? 1 : (cap.Max - cap.Min) / 20,
                        Margin = new Thickness(0, 0, 0, 6)
                    };
                    slider.ValueChanged += (_, args) =>
                    {
                        double v = isInt ? Math.Round(args.NewValue) : args.NewValue;
                        valText.Text = $"{cap.Label}: {v.ToString(cap.Fmt)}";
                        curVals[capIdx] = v;
                        RebuildEffects();
                    };
                    ctrlStack.Children.Add(valText);
                    ctrlStack.Children.Add(slider);
                }

                def.ExtraControls?.Invoke(ctrlStack, RebuildEffects);

                var ctrlBorder = new Border
                {
                    Background = ctrlPanelBg, CornerRadius = new CornerRadius(6),
                    Padding = new Thickness(14), Margin = new Thickness(0, 0, 12, 0),
                    VerticalAlignment = VerticalAlignment.Top,
                    Child = ctrlStack
                };
                Grid.SetColumn(ctrlBorder, 0);
                row.Children.Add(ctrlBorder);
            }
            else
            {
                row.ColumnDefinitions.Add(new ColumnDefinition
                    { Width = new GridLength(1, GridUnitType.Star) });
            }

            // Main preview
            int previewCol = hasControls ? 1 : 0;
            var previewImg = new System.Windows.Controls.Image
            {
                Source = GetCachedBitmap(imageCatalog[selImgIdx].Path, 800),
                Stretch = Stretch.UniformToFill,
                Width = 380, Height = 260,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };
            var previewBorder = new Border
            {
                Width = 500, Height = 340,
                HorizontalAlignment = HorizontalAlignment.Center,
                Child = previewImg
            };
            var fxPreview = MakeFx(previewImg);
            if (fxPreview != null) previewImg.Effect = fxPreview;

            Grid.SetColumn(previewBorder, previewCol);
            row.Children.Add(previewBorder);
            sectionStack.Children.Add(row);

            // ── Gallery ──────────────────────────────────────────
            sectionStack.Children.Add(new TextBlock
            {
                Text = "Applied to various targets:",
                FontSize = 12, Foreground = Brushes.Gray,
                Margin = new Thickness(0, 2, 0, 6)
            });
            var gallery = new WrapPanel();
            gallery.Children.Add(MakeCard("Original (no fx)",
                MakeImg("Images/landscape_1920.jpg", 120, 80), 140, 120));
            foreach (var (lbl, make, cw, ch) in galleryTargets)
            {
                var el = make();
                var fx = MakeFx(el);
                if (fx != null) el.Effect = fx;
                gallery.Children.Add(MakeCard(lbl, el, cw, ch));
            }
            sectionStack.Children.Add(gallery);

            sectionBorder.Child = sectionStack;
            contentHost.Children.Add(sectionBorder);
        }

        // Wire up selectors
        effectCombo.SelectionChanged += (_, _) => RebuildContent();
        imgCombo.SelectionChanged += (_, _) =>
        {
            selImgIdx = imgCombo.SelectedIndex;
            RebuildContent();
        };

        // Initial build
        RebuildContent();
    }

    // ══════════════════════════════════════════════════════════════
    //  Slider change handlers — rebuild tab contents dynamically
    // ══════════════════════════════════════════════════════════════
    private void ParticleSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        int count = (int)e.NewValue;
        ParticleCountLabel.Text = count.ToString();

        // Unhook render callback, clear, rebuild
        CompositionTarget.Rendering -= MoveParticles;
        ParticleCanvas.Children.Clear();
        _particles.Clear();
        _velocities.Clear();
        BuildParticleSwarm(count);
        UpdateShapeCount();
    }

    private void GradientSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        int count = (int)e.NewValue;
        GradientCountLabel.Text = count.ToString();
        GradientGrid.Children.Clear();
        BuildGradientGrid(count);
        UpdateShapeCount();
    }

    private void EffectsSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        int count = (int)e.NewValue;
        EffectsCountLabel.Text = count.ToString();
        EffectsPanel.Children.Clear();
        BuildEffectsPanel(count);
        UpdateShapeCount();
    }

    private void TransformSlider_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_isLoaded) return;
        int count = (int)e.NewValue;
        TransformCountLabel.Text = count.ToString();
        TransformCanvas.Children.Clear();
        _spinners.Clear();
        BuildTransformCanvas(count);
        UpdateShapeCount();
    }

    private void UpdateShapeCount()
    {
        _totalShapes = AnimCanvas.Children.Count
                     + ParticleCanvas.Children.Count
                     + GradientGrid.Children.Count
                     + EffectsPanel.Children.Count
                     + TextPanel.Children.Count
                     + ImagePanel.Children.Count
                     + TransformCanvas.Children.Count
                     + D2DEffectsPanel.Children.Count;
        ShapeCountText.Text = $"Shapes: {_totalShapes}";
    }

    // ══════════════════════════════════════════════════════════════
    //  FPS Helpers
    // ══════════════════════════════════════════════════════════════
    private void OnRendering(object? sender, EventArgs e) => _frameCount++;

    private void OnFpsTick(object? sender, EventArgs e)
    {
        double elapsed = _sw.Elapsed.TotalSeconds;
        _sw.Restart();
        double fps = elapsed > 0 ? _frameCount / elapsed : 0;
        FpsText.Text = fps.ToString("F1");
        FpsText.Foreground = fps < 30
            ? Brushes.OrangeRed
            : fps < 55 ? Brushes.Gold
            : new SolidColorBrush(Color.FromRgb(0x53, 0xc0, 0xb4));
        _frameCount = 0;
    }

    // ══════════════════════════════════════════════════════════════
    //  Animation helper
    // ══════════════════════════════════════════════════════════════
    private static void Animate(Animatable target, DependencyProperty prop,
                                double from, double to, double seconds,
                                bool autoReverse = false, bool forever = false)
    {
        var anim = new DoubleAnimation(from, to, TimeSpan.FromSeconds(seconds));
        if (autoReverse) anim.AutoReverse = true;
        if (forever) anim.RepeatBehavior = RepeatBehavior.Forever;
        anim.EasingFunction = new SineEase();
        target.BeginAnimation(prop, anim);
    }

    private static void AnimateElement(UIElement target, DependencyProperty prop,
                                       double from, double to, double seconds,
                                       bool autoReverse = false, bool forever = false)
    {
        var anim = new DoubleAnimation(from, to, TimeSpan.FromSeconds(seconds));
        if (autoReverse) anim.AutoReverse = true;
        if (forever) anim.RepeatBehavior = RepeatBehavior.Forever;
        anim.EasingFunction = new SineEase();
        target.BeginAnimation(prop, anim);
    }
}

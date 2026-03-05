using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;

namespace WpfAnimatedShapes;

public partial class MainWindow : Window
{
    private static readonly Random Rng = new();
    private int _shapeCount;

    public MainWindow()
    {
        // Enable D2D/Direct3D 11 rendering backend (opt-in via AppContext switch)
        // When WPF_D2D_ENABLED is compiled into the native layer, this switch
        // causes the render target factory to create CD2DDisplayRenderTarget
        // instead of CHwDisplayRenderTarget (D3D9).
        AppContext.SetSwitch("Switch.System.Windows.Media.UseD2DRendering", true);

        InitializeComponent();
        Loaded += OnLoaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Display info about the WPF assemblies we're running on
        var presCore = typeof(UIElement).Assembly;
        var presFramework = typeof(Window).Assembly;
        var windowsBase = typeof(DependencyObject).Assembly;

        // Check if D2D rendering is active
        bool d2dEnabled = AppContext.TryGetSwitch("Switch.System.Windows.Media.UseD2DRendering", out bool isEnabled) && isEnabled;
        string renderBackend = d2dEnabled ? "D2D/D3D11" : "D3D9 (classic)";

        FrameworkInfoText.Text =
            $"Renderer: {renderBackend} | " +
            $"PresentationCore: {presCore.GetName().Version} | " +
            $"PresentationFramework: {presFramework.GetName().Version} | " +
            $"WindowsBase: {windowsBase.GetName().Version} | " +
            $"Location: {System.IO.Path.GetDirectoryName(presCore.Location)}";

        // Start with some shapes
        AddRandomShapes(40);
    }

    private void AddShapesButton_Click(object sender, RoutedEventArgs e) => AddRandomShapes(20);

    private void ClearButton_Click(object sender, RoutedEventArgs e)
    {
        ShapeCanvas.Children.Clear();
        _shapeCount = 0;
        ShapeCountText.Text = "Shapes: 0";
    }

    private void AddRandomShapes(int count)
    {
        double canvasW = Math.Max(ShapeCanvas.ActualWidth, 1100);
        double canvasH = Math.Max(ShapeCanvas.ActualHeight, 700);

        for (int i = 0; i < count; i++)
        {
            Shape shape = CreateRandomShape();
            double size = Rng.Next(20, 100);

            // Random position
            double x = Rng.NextDouble() * (canvasW - size);
            double y = Rng.NextDouble() * (canvasH - size) + 40; // avoid buttons

            Canvas.SetLeft(shape, x);
            Canvas.SetTop(shape, y);

            // Set a RenderTransform for rotation & scale
            var transformGroup = new TransformGroup();
            var scaleTransform = new ScaleTransform(1, 1);
            var rotateTransform = new RotateTransform(0, size / 2, size / 2);
            transformGroup.Children.Add(scaleTransform);
            transformGroup.Children.Add(rotateTransform);
            shape.RenderTransform = transformGroup;

            ShapeCanvas.Children.Add(shape);
            _shapeCount++;

            // Apply animations
            AnimatePosition(shape, canvasW, canvasH, size);
            AnimateRotation(rotateTransform);
            AnimateScale(scaleTransform);
            AnimateOpacity(shape);
            AnimateColor(shape);
        }

        ShapeCountText.Text = $"Shapes: {_shapeCount}";
    }

    private Shape CreateRandomShape()
    {
        int type = Rng.Next(5);
        double size = Rng.Next(20, 100);
        Brush fill = RandomBrush();
        Brush stroke = RandomBrush();
        double strokeThickness = Rng.Next(0, 4);

        return type switch
        {
            0 => new Ellipse
            {
                Width = size,
                Height = size * (0.5 + Rng.NextDouble()),
                Fill = fill,
                Stroke = stroke,
                StrokeThickness = strokeThickness
            },
            1 => new Rectangle
            {
                Width = size,
                Height = size * (0.5 + Rng.NextDouble()),
                Fill = fill,
                Stroke = stroke,
                StrokeThickness = strokeThickness,
                RadiusX = Rng.Next(0, 15),
                RadiusY = Rng.Next(0, 15)
            },
            2 => CreateTriangle(size, fill, stroke, strokeThickness),
            3 => CreateStar(size, fill, stroke, strokeThickness),
            _ => CreateHexagon(size, fill, stroke, strokeThickness)
        };
    }

    private Polygon CreateTriangle(double size, Brush fill, Brush stroke, double strokeThickness)
    {
        return new Polygon
        {
            Points = new PointCollection
            {
                new Point(size / 2, 0),
                new Point(size, size),
                new Point(0, size)
            },
            Fill = fill,
            Stroke = stroke,
            StrokeThickness = strokeThickness,
            Width = size,
            Height = size
        };
    }

    private Polygon CreateStar(double size, Brush fill, Brush stroke, double strokeThickness)
    {
        var points = new PointCollection();
        int tips = Rng.Next(4, 9);
        double cx = size / 2, cy = size / 2;
        double outerR = size / 2, innerR = size / 4;

        for (int i = 0; i < tips * 2; i++)
        {
            double angle = Math.PI * i / tips - Math.PI / 2;
            double r = (i % 2 == 0) ? outerR : innerR;
            points.Add(new Point(cx + r * Math.Cos(angle), cy + r * Math.Sin(angle)));
        }

        return new Polygon
        {
            Points = points,
            Fill = fill,
            Stroke = stroke,
            StrokeThickness = strokeThickness,
            Width = size,
            Height = size
        };
    }

    private Polygon CreateHexagon(double size, Brush fill, Brush stroke, double strokeThickness)
    {
        var points = new PointCollection();
        double cx = size / 2, cy = size / 2, r = size / 2;

        for (int i = 0; i < 6; i++)
        {
            double angle = Math.PI / 3 * i - Math.PI / 2;
            points.Add(new Point(cx + r * Math.Cos(angle), cy + r * Math.Sin(angle)));
        }

        return new Polygon
        {
            Points = points,
            Fill = fill,
            Stroke = stroke,
            StrokeThickness = strokeThickness,
            Width = size,
            Height = size
        };
    }

    private void AnimatePosition(Shape shape, double canvasW, double canvasH, double size)
    {
        var duration = TimeSpan.FromSeconds(Rng.Next(3, 10));
        var ease = new SineEase { EasingMode = EasingMode.EaseInOut };

        // Horizontal drift
        var xAnim = new DoubleAnimation
        {
            To = Rng.NextDouble() * (canvasW - size),
            Duration = duration,
            EasingFunction = ease,
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever
        };

        // Vertical drift
        var yAnim = new DoubleAnimation
        {
            To = Rng.NextDouble() * (canvasH - size) + 40,
            Duration = TimeSpan.FromSeconds(Rng.Next(3, 10)),
            EasingFunction = ease,
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever
        };

        shape.BeginAnimation(Canvas.LeftProperty, xAnim);
        shape.BeginAnimation(Canvas.TopProperty, yAnim);
    }

    private void AnimateRotation(RotateTransform rotateTransform)
    {
        double targetAngle = Rng.Next(-720, 720);
        var rotAnim = new DoubleAnimation
        {
            To = targetAngle,
            Duration = TimeSpan.FromSeconds(Rng.Next(2, 12)),
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        rotateTransform.BeginAnimation(RotateTransform.AngleProperty, rotAnim);
    }

    private void AnimateScale(ScaleTransform scaleTransform)
    {
        double targetScale = 0.3 + Rng.NextDouble() * 1.5;
        var dur = TimeSpan.FromSeconds(Rng.Next(2, 8));
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseInOut };

        var scaleXAnim = new DoubleAnimation
        {
            To = targetScale,
            Duration = dur,
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = ease
        };
        var scaleYAnim = new DoubleAnimation
        {
            To = 0.3 + Rng.NextDouble() * 1.5,
            Duration = TimeSpan.FromSeconds(Rng.Next(2, 8)),
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = ease
        };

        scaleTransform.BeginAnimation(ScaleTransform.ScaleXProperty, scaleXAnim);
        scaleTransform.BeginAnimation(ScaleTransform.ScaleYProperty, scaleYAnim);
    }

    private void AnimateOpacity(Shape shape)
    {
        var opacityAnim = new DoubleAnimation
        {
            From = 0.4 + Rng.NextDouble() * 0.6,
            To = 0.2 + Rng.NextDouble() * 0.8,
            Duration = TimeSpan.FromSeconds(Rng.Next(2, 7)),
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new SineEase()
        };
        shape.BeginAnimation(UIElement.OpacityProperty, opacityAnim);
    }

    private void AnimateColor(Shape shape)
    {
        if (shape.Fill is SolidColorBrush fillBrush)
        {
            // Need a non-frozen brush so we can animate it
            var animBrush = new SolidColorBrush(fillBrush.Color);
            shape.Fill = animBrush;

            var colorAnim = new ColorAnimation
            {
                To = RandomColor(),
                Duration = TimeSpan.FromSeconds(Rng.Next(3, 10)),
                AutoReverse = true,
                RepeatBehavior = RepeatBehavior.Forever,
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            animBrush.BeginAnimation(SolidColorBrush.ColorProperty, colorAnim);
        }
    }

    private static Brush RandomBrush()
    {
        int style = Rng.Next(3);
        if (style == 0)
        {
            return new SolidColorBrush(RandomColor());
        }
        else if (style == 1)
        {
            return new LinearGradientBrush(
                RandomColor(), RandomColor(),
                Rng.NextDouble() * 360);
        }
        else
        {
            return new RadialGradientBrush(RandomColor(), RandomColor());
        }
    }

    private static Color RandomColor()
    {
        return Color.FromArgb(
            (byte)Rng.Next(150, 256),
            (byte)Rng.Next(0, 256),
            (byte)Rng.Next(0, 256),
            (byte)Rng.Next(0, 256));
    }
}
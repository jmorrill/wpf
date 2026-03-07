// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Blend modes for the D2DBlendEffect.
    /// </summary>
    public enum D2DBlendMode
    {
        Multiply = 0,
        Screen = 1,
        Darken = 2,
        Lighten = 3,
        Dissolve = 4,
        ColorBurn = 5,
        LinearBurn = 6,
        DarkerColor = 7,
        LighterColor = 8,
        ColorDodge = 9,
        LinearDodge = 10,
        Overlay = 11,
        SoftLight = 12,
        HardLight = 13,
        VividLight = 14,
        LinearLight = 15,
        PinLight = 16,
        HardMix = 17,
        Difference = 18,
        Exclusion = 19,
        Hue = 20,
        Saturation = 21,
        Color = 22,
        Luminosity = 23,
        Subtract = 24,
        Division = 25,
    }

    /// <summary>
    /// D2D Blend effect. Blends two inputs using a specified blend mode.
    /// Input 0 = Destination, Input 1 = Source.
    /// </summary>
    public class D2DBlendEffect : D2DEffect
    {
        private const int D2D1_BLEND_PROP_MODE = 0;

        public D2DBlendEffect()
        {
            EffectId = D2DEffects.Blend;
            BlendMode = D2DBlendMode.Multiply;
        }

        /// <summary>
        /// The blend mode to use. Default is Multiply.
        /// </summary>
        public D2DBlendMode BlendMode
        {
            get => _blendMode;
            set
            {
                _blendMode = value;
                SetValue(D2D1_BLEND_PROP_MODE, (int)value);
            }
        }

        /// <summary>
        /// Sets the destination (background) input.
        /// </summary>
        public Brush Destination
        {
            set => SetInput(0, value);
        }

        /// <summary>
        /// Sets the source (foreground) input.
        /// </summary>
        public Brush Source
        {
            set => SetInput(1, value);
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DBlendEffect();
        }

        private D2DBlendMode _blendMode = D2DBlendMode.Multiply;
    }
}

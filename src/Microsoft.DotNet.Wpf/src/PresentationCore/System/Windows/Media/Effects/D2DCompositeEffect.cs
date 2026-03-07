// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Composite modes.
    /// </summary>
    public enum D2DCompositeMode
    {
        SourceOver = 0,
        DestinationOver = 1,
        SourceIn = 2,
        DestinationIn = 3,
        SourceOut = 4,
        DestinationOut = 5,
        SourceAtop = 6,
        DestinationAtop = 7,
        Xor = 8,
        Plus = 9,
        SourceCopy = 10,
        BoundedSourceCopy = 11,
        MaskInvert = 12,
    }

    /// <summary>
    /// D2D Composite effect. Composites N inputs using a specified mode.
    /// Supports arbitrary number of inputs.
    /// </summary>
    public class D2DCompositeEffect : D2DEffect
    {
        private const int D2D1_COMPOSITE_PROP_MODE = 0;

        public D2DCompositeEffect()
        {
            EffectId = D2DEffects.Composite;
            Mode = D2DCompositeMode.SourceOver;
        }

        /// <summary>
        /// The composite mode. Default is SourceOver.
        /// </summary>
        public D2DCompositeMode Mode
        {
            get => _mode;
            set
            {
                _mode = value;
                SetValue(D2D1_COMPOSITE_PROP_MODE, (int)value);
            }
        }

        /// <summary>
        /// Adds a brush input at the next available index.
        /// </summary>
        public void AddInput(Brush brush)
        {
            SetInput(_nextInputIndex++, brush);
        }

        /// <summary>
        /// Adds an effect input at the next available index.
        /// </summary>
        public void AddInput(D2DEffect effect)
        {
            SetInput(_nextInputIndex++, effect);
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DCompositeEffect();
        }

        private D2DCompositeMode _mode = D2DCompositeMode.SourceOver;
        private int _nextInputIndex;
    }
}

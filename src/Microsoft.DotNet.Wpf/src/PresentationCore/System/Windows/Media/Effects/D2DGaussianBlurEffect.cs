// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Gaussian blur effect. Applies a Gaussian blur to the input.
    /// </summary>
    public class D2DGaussianBlurEffect : D2DEffect
    {
        // D2D1_GAUSSIANBLUR_PROP indices
        private const int D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION = 0;
        private const int D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION = 1;
        private const int D2D1_GAUSSIANBLUR_PROP_BORDER_MODE = 2;

        public D2DGaussianBlurEffect()
        {
            EffectId = D2DEffects.GaussianBlur;
            StandardDeviation = 3.0f;
        }

        /// <summary>
        /// The standard deviation (sigma) of the blur, in DIPs. Default is 3.0.
        /// </summary>
        public float StandardDeviation
        {
            get => _standardDeviation;
            set
            {
                _standardDeviation = value;
                SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, value);
                EffectPadding = value * 3.0;
            }
        }

        /// <summary>
        /// Border mode. 0 = Soft (default), 1 = Hard.
        /// </summary>
        public int BorderMode
        {
            get => _borderMode;
            set
            {
                _borderMode = value;
                SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, value);
            }
        }

        /// <summary>
        /// Optimization. 0 = Speed, 1 = Balanced (default), 2 = Quality.
        /// </summary>
        public int Optimization
        {
            get => _optimization;
            set
            {
                _optimization = value;
                SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, value);
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DGaussianBlurEffect();
        }

        private float _standardDeviation = 3.0f;
        private int _borderMode = 0;
        private int _optimization = 1;
    }
}

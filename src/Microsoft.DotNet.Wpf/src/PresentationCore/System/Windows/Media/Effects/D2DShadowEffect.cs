// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Shadow effect. Generates a shadow of the input.
    /// </summary>
    public class D2DShadowEffect : D2DEffect
    {
        private const int D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION = 0;
        private const int D2D1_SHADOW_PROP_COLOR = 1;
        private const int D2D1_SHADOW_PROP_OPTIMIZATION = 2;

        public D2DShadowEffect()
        {
            EffectId = D2DEffects.Shadow;
            BlurStandardDeviation = 3.0f;
            ShadowColor = Colors.Black;
        }

        /// <summary>
        /// The blur standard deviation for the shadow. Default is 3.0.
        /// </summary>
        public float BlurStandardDeviation
        {
            get => _blurStdDev;
            set
            {
                _blurStdDev = value;
                SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, value);
                EffectPadding = value * 3.0;
            }
        }

        /// <summary>
        /// The shadow color. Default is Black.
        /// </summary>
        public Color ShadowColor
        {
            get => _shadowColor;
            set
            {
                _shadowColor = value;
                SetValue(D2D1_SHADOW_PROP_COLOR,
                    value.ScR, value.ScG, value.ScB, value.ScA);
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
                SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, value);
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DShadowEffect();
        }

        private float _blurStdDev = 3.0f;
        private Color _shadowColor = Colors.Black;
        private int _optimization = 1;
    }
}

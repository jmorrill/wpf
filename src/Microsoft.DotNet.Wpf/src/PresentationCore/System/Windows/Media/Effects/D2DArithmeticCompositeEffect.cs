// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Arithmetic Composite effect.
    /// Output = C1*Source1*Source2 + C2*Source1 + C3*Source2 + C4.
    /// Input 0 = Source1, Input 1 = Source2.
    /// </summary>
    public class D2DArithmeticCompositeEffect : D2DEffect
    {
        private const int D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS = 0;
        private const int D2D1_ARITHMETICCOMPOSITE_PROP_CLAMP_OUTPUT = 1;

        public D2DArithmeticCompositeEffect()
        {
            EffectId = D2DEffects.ArithmeticComposite;
            SetCoefficients(0, 1, 1, 0);
        }

        /// <summary>
        /// Sets the arithmetic coefficients: C1*Src1*Src2 + C2*Src1 + C3*Src2 + C4.
        /// </summary>
        public void SetCoefficients(float c1, float c2, float c3, float c4)
        {
            _c1 = c1; _c2 = c2; _c3 = c3; _c4 = c4;
            SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS, c1, c2, c3, c4);
        }

        /// <summary>
        /// Whether to clamp output to [0,1]. Default is false.
        /// </summary>
        public bool ClampOutput
        {
            get => _clampOutput;
            set
            {
                _clampOutput = value;
                SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_CLAMP_OUTPUT, value);
            }
        }

        /// <summary>
        /// Sets Source1 input.
        /// </summary>
        public Brush Source1
        {
            set => SetInput(0, value);
        }

        /// <summary>
        /// Sets Source2 input.
        /// </summary>
        public Brush Source2
        {
            set => SetInput(1, value);
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DArithmeticCompositeEffect();
        }

        private float _c1, _c2, _c3, _c4;
        private bool _clampOutput;
    }
}

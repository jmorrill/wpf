// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Saturation effect. Adjusts the saturation of the input.
    /// </summary>
    public class D2DSaturationEffect : D2DEffect
    {
        private const int D2D1_SATURATION_PROP_SATURATION = 0;

        public D2DSaturationEffect()
        {
            EffectId = D2DEffects.Saturation;
            Saturation = 0.5f;
        }

        /// <summary>
        /// Saturation level. 0 = fully desaturated, 1 = fully saturated. Default 0.5.
        /// </summary>
        public float Saturation
        {
            get => _saturation;
            set
            {
                _saturation = value;
                SetValue(D2D1_SATURATION_PROP_SATURATION, value);
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DSaturationEffect();
        }

        private float _saturation = 0.5f;
    }
}

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D CrossFade effect. Blends between two inputs based on a weight.
    /// Input 0 = Destination, Input 1 = Source.
    /// </summary>
    public class D2DCrossFadeEffect : D2DEffect
    {
        private const int D2D1_CROSSFADE_PROP_WEIGHT = 0;

        public D2DCrossFadeEffect()
        {
            EffectId = D2DEffects.CrossFade;
            Weight = 0.5f;
        }

        /// <summary>
        /// Weight of the cross-fade. 0 = fully destination, 1 = fully source.
        /// Default is 0.5.
        /// </summary>
        public float Weight
        {
            get => _weight;
            set
            {
                _weight = value;
                SetValue(D2D1_CROSSFADE_PROP_WEIGHT, value);
            }
        }

        /// <summary>
        /// Sets the destination input.
        /// </summary>
        public Brush Destination
        {
            set => SetInput(0, value);
        }

        /// <summary>
        /// Sets the source input.
        /// </summary>
        public Brush Source
        {
            set => SetInput(1, value);
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DCrossFadeEffect();
        }

        private float _weight = 0.5f;
    }
}

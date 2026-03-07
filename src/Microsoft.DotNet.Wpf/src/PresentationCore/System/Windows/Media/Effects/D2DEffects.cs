// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// Contains CLSID constants for Direct2D built-in effects.
    /// </summary>
    public static class D2DEffects
    {
        // Single-input effects
        public static readonly Guid GaussianBlur = new Guid("1feb6d69-2fe6-4ac9-8c58-1d7f93e7a6a5");
        public static readonly Guid Saturation = new Guid("5cb2d9cf-327d-459f-a0ce-40c0b2086bf7");
        public static readonly Guid HueRotation = new Guid("0f4458ec-4b32-491b-9e85-bd73f44d3eb6");
        public static readonly Guid ColorMatrix = new Guid("921f03d6-641c-47df-852d-b4bb6153ae11");
        public static readonly Guid Shadow = new Guid("c67ea361-1863-4e69-89db-695d3e9a5b6b");
        public static readonly Guid Sepia = new Guid("3a1af410-5f1d-4dbe-84df-915da79b7153");
        public static readonly Guid Grayscale = new Guid("36dde0eb-3725-42e0-836d-52fb20aee644");
        public static readonly Guid Invert = new Guid("e0c3784d-cb39-4e84-b6fd-6b72f0810263");
        public static readonly Guid Brightness = new Guid("8cea8d1e-77b0-4986-b3b9-2f0c0eae7887");
        public static readonly Guid Contrast = new Guid("b648a78a-0ed5-4f80-a94a-8e825aca6b77");
        public static readonly Guid Exposure = new Guid("b56c8cfa-f634-41ee-bee0-ffa617106004");
        public static readonly Guid Sharpen = new Guid("c9b887cb-c5ff-4dc5-9779-273dcf417c7d");
        public static readonly Guid Vignette = new Guid("c00c40be-5e67-4ca3-95b4-f4b02c115135");
        public static readonly Guid TemperatureAndTint = new Guid("89176087-8af9-4a08-aeb1-895f38db1766");
        public static readonly Guid Straighten = new Guid("4da47b12-79a3-4fb0-8237-bbc3b2a4de08");
        public static readonly Guid HighlightsAndShadows = new Guid("cadc8384-323f-4c7e-a361-2e2b24df6ee4");
        public static readonly Guid Posterize = new Guid("2188945e-33a3-4366-b7bc-086bd02d0884");

        // Multi-input effects
        public static readonly Guid Blend = new Guid("81c5b77b-13f8-4cdd-ad20-c890547ac65d");
        public static readonly Guid Composite = new Guid("48fc9f51-f6ac-48f1-8b58-3b28ac46f76d");
        public static readonly Guid CrossFade = new Guid("12f575e8-4db1-485f-9a84-03a07dd3829f");
        public static readonly Guid ArithmeticComposite = new Guid("fc151437-049a-4784-a24a-f1c4daf20987");

        // Geometry / transform effects
        public static readonly Guid Crop = new Guid("e23f7110-0e9a-4324-af47-6a2c0c46f35b");
        public static readonly Guid Scale = new Guid("9daf9c52-3df0-44b4-b2e1-4a3c9a7ff152");
        public static readonly Guid AffineTransform2D = new Guid("6aa97485-6354-4cfc-908c-e4a74f62c96c");
        public static readonly Guid PerspectiveTransform3D = new Guid("c2844d0b-3d86-46e7-85ba-526c9240f3fb");
        public static readonly Guid Border = new Guid("2a2d49c0-4acf-43c7-8c6a-7c4a27874d27");
    }
}

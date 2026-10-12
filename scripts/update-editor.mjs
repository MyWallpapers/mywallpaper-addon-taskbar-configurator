import { readFileSync, writeFileSync } from 'node:fs'
const path = new URL('../manifest.json', import.meta.url)
const manifest = JSON.parse(readFileSync(path, 'utf8'))
const text = (title, extra = {}) => ({ type: 'string', title, ...extra })
const number = (title, minimum, maximum, value) => ({ type: 'number', title, minimum, maximum, ...(value === undefined ? {} : { default: value }) })
const color = title => text(title, { format: 'color', default: '#202020FF' })
const object = (title, properties, required = [], extra = {}) => ({ type: 'object', title, properties, required, additionalProperties: false, ...extra })
const material = (title, kind, properties) => object(title, { kind: { const: kind }, ...properties }, ['kind', ...Object.keys(properties).filter(key => properties[key].default !== undefined)])
const selector = object('Target', {
  semantic: { type: 'string', title: 'Element', enum: ['surface','button','startButton','search','widgets','systemTray','clock','indicator','icon','label'], enumNames: ['Taskbar surface','Application buttons','Start','Search','Widgets','System tray','Clock','Activity indicators','Icons','Labels'], default: 'surface' },
  name: text('Exact element name'), type: text('XAML class'), ancestorName: text('Named ancestor'),
  exactType: { type: 'boolean', title: 'Exact class match' }, root: { type: 'boolean', title: 'Visual root only' },
  index: number('Child index (1 = first)', 0, 4096),
  where: { type: 'object', title: 'Property filters', additionalProperties: {} },
  parents: { type: 'array', title: 'Direct parents (nearest first)', maxItems: 16, items: object('Parent', { name: text('Name'), type: text('XAML class') }, [], { default: { type: 'Grid' } }) },
}, [], { default: { semantic: 'surface' },anyOf:[{required:['semantic']},{required:['name']},{required:['type']}] })
const value = { oneOf: [
  number('Number'), text('Text or native enum'), { type: 'boolean', title: 'Boolean' },
  { type: 'array', title: 'Sides / corners / origin', items: { type: 'number' }, maxItems: 4, default: [0,0,0,0] },
  material('Solid color', 'solid', { color: color('Color') }),
  material('Gradient', 'gradient', { stops: { type: 'array', title: 'Color stops', minItems:2,maxItems: 64, default: [{offset:0,color:'#202020FF'},{offset:1,color:'#606060FF'}], items: object('Stop', { offset: number('Position',0,1,0),color:color('Color') }, ['offset','color']) }, start: { type:'array',title:'Start point',items:{type:'number'},minItems:2,maxItems:2,default:[0,0] },end:{type:'array',title:'End point',items:{type:'number'},minItems:2,maxItems:2,default:[1,1]} }),
  material('Image', 'image', { source: text('File URI or image URL', { minLength:1,default: '' }), stretch: { type:'string',title:'Image fit',enum:['UniformToFill','Uniform','Fill','None'],default:'UniformToFill' } }),
  material('Windows acrylic', 'acrylic', {color:color('Tint'),tintOpacity:number('Tint opacity',0,1,0.5),fallbackColor:color('Fallback color')}),
  material('Backdrop blur', 'blur', {color:color('Tint'),radius:number('Blur radius',0,100,18),tintOpacity:number('Tint opacity',0,1,0.35),saturation:number('Saturation',0,4,1),luminosity:number('Luminosity',0,2,1),noiseOpacity:number('Grain opacity',0,0.25,0),fallbackColor:color('Fallback color')}),
  material('2D transformation', 'transform', {scaleX:number('Horizontal scale',-10,10,1),scaleY:number('Vertical scale',-10,10,1),rotation:number('Rotation (degrees)',-360,360,0),translateX:number('Horizontal offset',-4096,4096,0),translateY:number('Vertical offset',-4096,4096,0),skewX:number('Horizontal skew',-89,89,0),skewY:number('Vertical skew',-89,89,0),centerX:number('Origin X'),centerY:number('Origin Y')}),
  material('3D transformation', 'transform3d', {scaleX:number('Scale X',-10,10,1),scaleY:number('Scale Y',-10,10,1),scaleZ:number('Scale Z',-10,10,1),rotationX:number('Rotation X (degrees)',-360,360,0),rotationY:number('Rotation Y (degrees)',-360,360,0),rotationZ:number('Rotation Z (degrees)',-360,360,0),translateX:number('Offset X',-4096,4096,0),translateY:number('Offset Y',-4096,4096,0),translateZ:number('Offset Z',-4096,4096,0),centerX:number('Origin X'),centerY:number('Origin Y'),centerZ:number('Origin Z')}),
  material('Local perspective', 'projection', {rotationX:number('Rotation X (degrees)',-360,360,0),rotationY:number('Rotation Y (degrees)',-360,360,0),rotationZ:number('Rotation Z (degrees)',-360,360,0),centerOfRotationX:number('Relative origin X',0,1,0.5),centerOfRotationY:number('Relative origin Y',0,1,0.5),centerOfRotationZ:number('Origin Z',-4096,4096,0),globalOffsetX:number('Scene offset X',-4096,4096,0),globalOffsetY:number('Scene offset Y',-4096,4096,0),globalOffsetZ:number('Scene offset Z',-4096,4096,0),localOffsetX:number('Local offset X',-4096,4096,0),localOffsetY:number('Local offset Y',-4096,4096,0),localOffsetZ:number('Local offset Z',-4096,4096,0)}),
  material('Shared perspective', 'perspective', {depth:number('Camera distance',1,100000,1000),offsetX:number('Viewport origin X',-4096,4096,0),offsetY:number('Viewport origin Y',-4096,4096,0)}),
  material('Grid columns', 'columns', {sizes:{type:'array',title:'Column widths',items:{type:'string'},maxItems:32,default:['Auto','*','Auto']}}),
  material('Grid rows', 'rows', {sizes:{type:'array',title:'Row heights',items:{type:'string'},maxItems:32,default:['Auto']}}),
  material('Brush transition', 'transition', {durationMs:number('Duration (ms)',0,5000,180)}),
  object('Shared variable', {ref:text('Variable name',{default:''})},['ref']),
  object('Theme resource', {resource:text('Resource name',{default:'SystemAccentColor'})},['resource']),
  object('Calculated value', {expression:text('Expression',{default:'max(0, taskbarWidth - 16)'})},['expression']),
  object('Native XAML object', {xaml:text('XAML fragment',{default:''})},['xaml']),
  {type:'null',title:'Clear local value',default:null},
] }
const nativeProperties = ['Background','Foreground','Fill','Stroke','Opacity','Visibility','Width','Height','MinWidth','MaxWidth','MinHeight','MaxHeight','Margin','Padding','CornerRadius','BorderBrush','BorderThickness','RadiusX','RadiusY','StrokeThickness','FontFamily','FontSize','FontWeight','FontStyle','Text','TextAlignment','TextTrimming','TextWrapping','HorizontalAlignment','VerticalAlignment','HorizontalContentAlignment','VerticalContentAlignment','RenderTransform','RenderTransformOrigin','Projection','Transform3D','IsHitTestVisible','IsEnabled','Source','Content','Shadow','BackgroundTransition','ForegroundTransition','ColumnDefinitions','RowDefinitions','Grid.Column','Grid.Row','Grid.ColumnSpan','Grid.RowSpan','Canvas.ZIndex','Canvas.Left','Canvas.Top','Transitions']
const brushes = new Set(['Background','Foreground','Fill','Stroke','BorderBrush'])
const brushValue={oneOf:value.oneOf.filter(branch => ['solid','gradient','image','acrylic','blur'].includes(branch.properties?.kind?.const) || ['Theme resource','Shared variable','Native XAML object','Clear local value'].includes(branch.title))}
const knownProperties = Object.fromEntries(nativeProperties.map(name => {
  if (name === 'Opacity') return [name,number(name,0,1,1)]
  if (name === 'IsEnabled' || name === 'IsHitTestVisible') return [name,{type:'boolean',title:name,default:true}]
  return [name,{title:name,$ref:brushes.has(name)?'#/$defs/brush':'#/$defs/value'}]
}))
const rule = object('Rule', {
  selector,
  properties:{type:'object',title:'Properties',properties:knownProperties,additionalProperties:value,minProperties:1,default:{Background:{kind:'solid',color:'#202020FF'}}},
  stateGroup:text('Windows visual state group',{minLength:1}),state:text('Windows visual state',{minLength:1}),enabled:{type:'boolean',title:'Enable this rule',default:true},
},['selector','properties'],{summaryFields:['selector.semantic','selector.name','selector.type','state'],dependentRequired:{state:['stateGroup'],stateGroup:['state']}})
const profile = object('Style rules', {
  schemaVersion:{type:'integer',title:'Profile version',enum:[1,2],default:2,hidden:true},
  rules:{type:'array',title:'Rules',maxItems:256,items:rule,default:[]},
  variables:{type:'object',title:'Shared values',additionalProperties:value},
  resources:{type:'object',title:'Theme values',additionalProperties:{oneOf:[value,object('Light / dark value',{light:value,dark:value},['light','dark'])]}},
  captures:{type:'array',title:'Read dimensions and properties',maxItems:128,items:object('Capture',{name:text('Variable name',{default:'taskbarWidth'}),selector,property:text('Property to read',{default:'ActualWidth'})},['name','selector','property'],{summaryFields:['name','property']})},
  animations:{type:'array',title:'Pointer animations',maxItems:128,items:object('Animation',{selector,hoverScale:number('Hover scale',0.5,1.5,1.06),pressScale:number('Pressed scale',0.5,1.5,0.96),hoverOpacity:number('Hover opacity',0,1,1),pressOpacity:number('Pressed opacity',0,1,0.92),hoverDurationMs:number('Hover duration (ms)',0,2000,180),pressDurationMs:number('Pressed duration (ms)',0,2000,90),easing:{type:'string',title:'Easing',enum:['linear','ease-out','ease-in-out'],default:'ease-out'}},['selector'],{summaryFields:['selector.semantic','selector.name']})},
},['schemaVersion','rules'])
profile.$defs={value,brush:brushValue}
manifest.version = '0.1.8'
manifest.description = 'Compose native Windows taskbar styles, materials, visual states, layout and animations directly in MyWallpaper.'
manifest.presets = []
for (const setting of manifest.settings) {
  if (setting.showIf?.setting === 'profile' || Object.hasOwn(setting.showIf ?? {}, 'profile')) delete setting.showIf
  // Older manifests use the canonical predicate {settingId, equals}.
  if (setting.showIf?.settingId === 'profile') delete setting.showIf
  if (setting.id === 'profile') { setting.label='Legacy fallback palette';setting.description='Only fills missing values from older configurations. Every saved control remains editable.';setting.parent='advancedSection' }
  if (setting.id === 'advancedSection') {setting.label='Compose your own rules';setting.description='Combine targets, materials, native visual states and layout. Guided editing and JSON use the same saved profile.'}
  if (setting.id === 'advancedProfileJson') {setting.label='Style rules';setting.description='Changes apply together. Leave this document empty to remove custom rules while keeping the other controls active. Native properties depend on the targeted Windows element; incompatible rules are reported in the add-on diagnostic log.';setting.editor={format:'json',blankIsDefault:true,schema:profile};setting.placeholder='';}
}
writeFileSync(path, JSON.stringify(manifest,null,2)+'\n')

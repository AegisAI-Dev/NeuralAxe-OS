import { ComponentFixture, TestBed } from '@angular/core/testing';

import { AppFooterComponent } from './app.footer.component';
import { provideHttpClient } from '@angular/common/http';

describe('AppFooterComponent', () => {
  let component: AppFooterComponent;
  let fixture: ComponentFixture<AppFooterComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [AppFooterComponent],
      providers: [provideHttpClient()]
    })
    .compileComponents();

    fixture = TestBed.createComponent(AppFooterComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should show the NeuralAxe OS product identity', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('NeuralAxe OS');
    expect(text).toContain('0.1.0-dev');
    expect(text).toContain('Development Build');
    expect(text).toContain('NeuralShield');
  });

  it('should attribute the upstream ESP-Miner / AxeOS projects', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('based on the open-source ESP-Miner and AxeOS projects');
    expect(text).toContain('v2.14.2');
    expect(text).toContain('GPL-3.0');
  });
});
